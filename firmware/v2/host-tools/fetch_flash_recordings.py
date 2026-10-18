#!/usr/bin/env python3
"""Download recordings from the SD-free flight recorder (flight_recorder_flash) over USB.

Default: fetch every recording not already in the output folder as raw Opus packets
(the fast "O" command), wrap them in an Ogg Opus file, and convert that to a 16 kHz
WAV with ffmpeg. --device-wav instead asks the recorder to decode to WAV itself
(the "G" command Easy Eject uses; slower, but needs no ffmpeg).

Examples:
  .venv/bin/python fetch_flash_recordings.py                 # fetch new recordings
  .venv/bin/python fetch_flash_recordings.py --list
  .venv/bin/python fetch_flash_recordings.py --status
  .venv/bin/python fetch_flash_recordings.py --name REC0003 --device-wav
  .venv/bin/python fetch_flash_recordings.py --cmd S          # start/stop by hand
  .venv/bin/python fetch_flash_recordings.py --wipe           # erase all recordings
  .venv/bin/python fetch_flash_recordings.py --set-clock      # bench: Mac time and zone

The recorder normally gets the time from the flight controller (Betaflight MSP_RTC,
set by GPS or the configurator). --set-clock gives it the Mac's time until the next
power-off, for bench tests, and stores the Mac's time zone for file names.
"""

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

DTX_PACKET = bytes([0x48])  # SILK WB 20 ms TOC with no frame data: decoder conceals it


def default_port() -> str:
    """The recorder's USB serial port, found by its USB product name (nothing is sent)."""
    ports = list(list_ports.comports())
    for port in ports:
        if port.product == "FPV Audio Recorder":
            return port.device
    # Firmware before v2 reports Espressif's generic name; its ID is 303a:1001.
    for port in ports:
        if port.vid == 0x303A and port.pid == 0x1001:
            return port.device
    raise RuntimeError("No recorder found on USB (other ports: "
                       + (", ".join(p.device for p in ports if p.vid) or "none") + ")")


def local_tz_rule() -> str:
    """A POSIX TZ rule for the Mac's zone (US Eastern unless the zone is UTC or fixed)."""
    offset = -time.timezone // 60
    if time.daylight == 0:
        hours, minutes = divmod(-offset, 60)
        return f"UTC{hours}" + (f":{minutes:02d}" if minutes else "")
    if offset == -300:
        return "EST5EDT,M3.2.0,M11.1.0"
    if offset == -360:
        return "CST6CDT,M3.2.0,M11.1.0"
    if offset == -420:
        return "MST7MDT,M3.2.0,M11.1.0"
    if offset == -480:
        return "PST8PDT,M3.2.0,M11.1.0"
    raise RuntimeError("Set the zone by hand: --cmd 'ZONE <POSIX TZ rule>'")


class Recorder:
    def __init__(self, port: str):
        self.dev = serial.Serial(port, 115200, timeout=2, write_timeout=5)
        time.sleep(0.3)
        self.dev.reset_input_buffer()

    def send(self, command: str) -> None:
        self.dev.write(command.encode() if len(command) == 1 else (command + "\n").encode())

    def read_line(self, timeout: float = 10) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.dev.readline().decode("ascii", errors="replace").strip()
            if line:
                return line
        raise TimeoutError("Recorder did not answer")

    def wait_for(self, pattern: str, timeout: float = 15, echo: bool = False) -> re.Match:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(max(0.1, deadline - time.monotonic()))
            if echo:
                print(line)
            match = re.fullmatch(pattern, line)
            if match:
                return match
            if line.startswith("ERROR"):
                raise RuntimeError(line)
        raise TimeoutError(f"Timed out waiting for {pattern}")

    def read_exact(self, count: int) -> bytes:
        data = bytearray()
        while len(data) < count:
            block = self.dev.read(min(65536, count - len(data)))
            if not block:
                raise TimeoutError(f"Transfer stopped after {len(data)} of {count} bytes")
            data.extend(block)
        return bytes(data)

    def list(self) -> list[tuple[str, int]]:
        self.send("L")
        self.wait_for("LIST_BEGIN")
        files = []
        while True:
            line = self.read_line()
            if line == "LIST_END":
                return files
            match = re.fullmatch(r"FILE /(REC\d{4})\.WAV (\d+)", line)
            if match:
                files.append((match.group(1), int(match.group(2))))


# ---------- Ogg Opus writer ----------

def _crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        table.append(r & 0xFFFFFFFF)
    return table


CRC_TABLE = _crc_table()


def ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) ^ CRC_TABLE[((crc >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return crc


def ogg_page(packets: list[bytes], granule: int, serial_no: int, seq: int, flags: int) -> bytes:
    lacing = bytearray()
    for p in packets:
        lacing.extend([255] * (len(p) // 255))
        lacing.append(len(p) % 255)
    header = struct.pack("<4sBBqIIIB", b"OggS", 0, flags, granule, serial_no, seq, 0, len(lacing))
    page = bytearray(header + lacing + b"".join(packets))
    struct.pack_into("<I", page, 22, ogg_crc(bytes(page)))
    return bytes(page)


def write_ogg_opus(path: Path, packets: list[bytes], preskip: int) -> None:
    serial_no = 0x5245434F
    head = b"OpusHead" + struct.pack("<BBHIhB", 1, 1, preskip, 16000, 0, 0)
    vendor = b"flight_recorder_flash"
    tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
    out = bytearray(ogg_page([head], 0, serial_no, 0, 0x02))
    out += ogg_page([tags], 0, serial_no, 1, 0)
    seq, granule = 2, 0
    for i in range(0, len(packets), 50):
        group = packets[i:i + 50]
        granule += 960 * len(group)
        last = i + 50 >= len(packets)
        out += ogg_page(group, granule, serial_no, seq, 0x04 if last else 0)
        seq += 1
    path.write_bytes(out)


# ---------- Fetch ----------

def fetch_opus(rec: Recorder, name: str, out_dir: Path) -> Path:
    rec.send(f"O/{name}.WAV")
    m = rec.wait_for(r"BEGIN_OPUS (\d+) (\d+) (\d+) /(REC\d{4})\.WAV", timeout=30)
    size, frames, preskip = int(m.group(1)), int(m.group(2)), int(m.group(3))
    started = time.monotonic()
    raw = rec.read_exact(size)
    elapsed = time.monotonic() - started
    packets, off, lost = [], 0, 0
    while off < len(raw) and len(packets) < frames:
        n = raw[off]
        packet = raw[off + 1:off + 1 + n]
        if n == 0:
            packet, lost = DTX_PACKET, lost + 1
        packets.append(packet)
        off += 1 + n
    if len(packets) != frames:
        raise RuntimeError(f"{name}: parsed {len(packets)} of {frames} frames")
    opus_path = out_dir / f"{name}.opus"
    write_ogg_opus(opus_path, packets, preskip)
    wav_path = out_dir / f"{name}.wav"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(opus_path), "-ar", "16000",
                    "-ac", "1", "-c:a", "pcm_s16le", str(wav_path)], check=True)
    print(f"{name}: {frames * 0.02:.1f} s, {size} bytes in {elapsed:.1f} s, lost frames {lost} -> "
          f"{wav_path.name} (+ {opus_path.name})")
    return wav_path


def fetch_wav(rec: Recorder, name: str, out_dir: Path) -> Path:
    rec.send(f"G/{name}.WAV")
    m = rec.wait_for(r"BEGIN (\d+) /(REC\d{4})\.WAV", timeout=30)
    size = int(m.group(1))
    started = time.monotonic()
    data = rec.read_exact(size)
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError("Download lacks RIFF/WAVE header")
    path = out_dir / f"{name}-device.wav"
    path.write_bytes(data)
    print(f"{name}: {size} bytes in {time.monotonic() - started:.1f} s -> {path.name} "
          f"(SHA-256 {hashlib.sha256(data).hexdigest()[:16]}...)")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).parent / "bench-results" / "flash-recordings")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--name", action="append", help="REC0001 etc.; repeatable (default: all new)")
    parser.add_argument("--all", action="store_true", help="re-fetch recordings already downloaded")
    parser.add_argument("--device-wav", action="store_true", help="have the recorder decode to WAV")
    parser.add_argument("--cmd", help="send a raw command (S, E, T, I, Q ...) and print replies")
    parser.add_argument("--wipe", action="store_true", help="erase every recording on the board")
    parser.add_argument("--set-clock", action="store_true",
                        help="send the Mac's time and time zone (bench; the FC clock wins in flight)")
    args = parser.parse_args()

    rec = Recorder(args.port or default_port())
    if args.set_clock:
        rec.send("ZONE " + local_tz_rule())
        rec.wait_for(r"ZONE .*", echo=True)
        rec.send(f"CLOCK {int(time.time() * 1000)}")
        rec.wait_for(r"CLOCK .*", echo=True)
        return 0
    if args.cmd:
        rec.send(args.cmd)
        end = time.monotonic() + 3
        while time.monotonic() < end:
            line = rec.dev.readline().decode("ascii", errors="replace").strip()
            if line:
                print(line)
        return 0
    if args.wipe:
        rec.send("WIPE")
        rec.wait_for(r"WIPE_DONE .*", timeout=180, echo=True)
        return 0
    if args.status:
        rec.send("Q")
        rec.wait_for(r"STATUS .*", echo=True)
        end = time.monotonic() + 1
        while time.monotonic() < end:
            line = rec.dev.readline().decode("ascii", errors="replace").strip()
            if line.startswith("REC "):
                print(line)
        return 0
    files = rec.list()
    if args.list:
        for name, size in files:
            print(f"{name}  {(size - 44) / 32000:.1f} s")
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    wanted = [n for n, _ in files]
    if args.name:
        wanted = [n.upper().removesuffix(".WAV") for n in args.name]
    elif not args.all:
        wanted = [n for n in wanted if not (args.output_dir / f"{n}.wav").exists()]
    if not wanted:
        print("Nothing new to fetch.")
    for name in wanted:
        (fetch_wav if args.device_wav else fetch_opus)(rec, name, args.output_dir)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, TimeoutError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
