#!/usr/bin/env python3
"""Pair FPV recorder WAVs with DJI O4 clips and line them up.

1. Pairing. Recorder WAVs from the v2 firmware carry their UTC start time (bext chunk,
   also in FLIGHTS.CSV); DJI clips carry creation_time (UTC, whole seconds). Each WAV is
   paired with every clip it overlaps. Older WAVs (SD firmware) have no clock: when
   RECLOG.TXT is next to them, their start times relative to each power-up come from it
   and one offset per power-up is found from the content; otherwise each WAV is
   searched against every clip.
2. Fine alignment. Motor RPM drives both the prop noise the recorder hears (80-300 Hz)
   and the vibration in the O4's 2 kHz IMU (djmd stream, read with tools/gyro2bb).
   The two energy curves are cross-correlated at 50 ms steps, refined to about 10 ms.
   The DJI clock and the GPS clock can differ by a few seconds, so one session offset
   is estimated from the confident pairs and used for the rest.
3. Output, in the output folder: one WAV per clip, exactly as long as the clip and
   starting at its first frame (drop it at the clip's start in any editor), and
   sync-report.csv. --mux also writes an MP4 copy with the audio added (main video
   stream only: ffmpeg can't carry DJI's gyro streams, so stabilize the original clip).

Usage:
  audio_video_sync.py AUDIO_DIR VIDEO_DIR [--out DIR] [--mux] [--search 120]
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import warnings
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as ss

warnings.filterwarnings("ignore", category=wavfile.WavFileWarning)  # bext chunk

HERE = Path(__file__).resolve().parent
GYRO2BB = HERE / "tools" / "gyro2bb"
HOP = 0.05            # feature step, s
MIN_OVERLAP = 6.0     # s of overlap for a lag to count
Z_SEARCH = 4.0        # content peak needed to match with no clock
Z_REFINE = 3.5        # content peak needed to move an offset found from the clocks
MIN_REFINE_OVERLAP = 20.0
MIN_SEARCH_OVERLAP = 45.0


@dataclass
class Audio:
    path: Path
    duration: float
    start_utc: float | None = None      # epoch seconds
    boot: str = ""
    relative_start: float | None = None  # legacy: seconds after power-up
    sr: int = 16000
    _pcm: np.ndarray | None = None
    _feat: np.ndarray | None = None

    def pcm(self) -> np.ndarray:
        if self._pcm is None:
            sr, data = wavfile.read(self.path)
            if data.ndim > 1:
                data = data.mean(1)
            self.sr = sr
            self._pcm = data.astype(np.float32)
        return self._pcm

    def feature(self) -> np.ndarray:
        if self._feat is None:
            self._feat = band_energy(self.pcm().astype(float), self.sr, 80, 300)
        return self._feat


@dataclass
class Video:
    path: Path
    duration: float
    start_utc: float
    _feat: np.ndarray | None = None
    has_gyro: bool = True

    def feature(self) -> np.ndarray | None:
        if self._feat is None and self.has_gyro:
            rate = o4_body_rate(self.path)
            if rate is None:
                self.has_gyro = False
                return None
            self._feat = band_energy(rate, 2000, 30, 450)
        return self._feat


@dataclass
class Match:
    audio: Audio
    video: Video
    offset: float          # audio starts at this video time (s); negative = before the clip
    corr: float = 0.0
    z: float = 0.0         # content peak against all lags; 4+ is a clear match
    method: str = ""
    overlap: float = 0.0


# ---------- reading ----------

def read_wav_meta(path: Path) -> tuple[float, float | None, str]:
    """Duration, UTC start (epoch s) from the bext description, and power-up id."""
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise ValueError(f"{path} is not a WAV")
        byte_rate, data_bytes, desc = 32000, 0, ""
        while True:
            chunk = f.read(8)
            if len(chunk) < 8:
                break
            cid, size = chunk[:4], struct.unpack("<I", chunk[4:])[0]
            if cid == b"fmt ":
                fmt = f.read(size)
                byte_rate = struct.unpack("<I", fmt[8:12])[0]
            elif cid == b"bext":
                desc = f.read(size)[:256].split(b"\0")[0].decode("ascii", "replace")
            elif cid == b"data":
                data_bytes = size
                break
            else:
                f.seek(size + (size & 1), 1)
                continue
            if size & 1:
                f.seek(1, 1)
    start = None
    m = re.search(r"start_utc=(\S+)Z", desc)
    if m:
        start = dt.datetime.fromisoformat(m.group(1)).replace(tzinfo=dt.timezone.utc).timestamp()
    boot = re.search(r"P(\d{4})", desc)
    return data_bytes / byte_rate, start, ("P" + boot.group(1)) if boot else ""


def read_reclog(audio_dir: Path) -> dict[str, tuple[str, float]]:
    """Legacy SD firmware log: file -> (power-up group, start seconds after power-up)."""
    log = audio_dir / "RECLOG.TXT"
    out: dict[str, tuple[str, float]] = {}
    if not log.exists():
        return out
    group = 0
    for line in log.read_text(errors="replace").splitlines():
        if line.strip() == "BOOT":
            group += 1
            continue
        parts = line.split()
        m = re.search(r"up_ms=(\d+)", line)
        if len(parts) >= 3 and m and parts[0].upper().endswith(".WAV"):
            seconds = int(parts[2]) / 32000.0
            out[parts[0].lstrip("/").upper()] = (f"boot{group}", int(m.group(1)) / 1000.0 - seconds)
    return out


def load_audio(audio_dir: Path) -> list[Audio]:
    reclog = read_reclog(audio_dir)
    clips = []
    for p in sorted(audio_dir.glob("*.[Ww][Aa][Vv]")):
        if p.name.startswith("._"):
            continue
        duration, start, boot = read_wav_meta(p)
        a = Audio(p, duration, start, boot)
        if start is None and p.name.upper() in reclog:
            a.boot, a.relative_start = reclog[p.name.upper()]
        clips.append(a)
    return clips


def load_videos(video_dir: Path) -> list[Video]:
    out = []
    for p in sorted(video_dir.glob("*.[Mm][Pp]4")):
        if p.name.startswith("._"):
            continue
        info = json.loads(subprocess.run(
            ["ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", str(p)],
            capture_output=True, text=True).stdout or "{}")
        streams = info.get("streams", [])
        durations = [float(s["duration"]) for s in streams if s.get("codec_type") == "video" and "duration" in s]
        duration = float(info.get("format", {}).get("duration", 0) or 0) or (max(durations) if durations else 0)
        created = info.get("format", {}).get("tags", {}).get("creation_time")
        if not created or duration <= 0:
            print(f"  skip {p.name}: no creation time or duration")
            continue
        start = dt.datetime.fromisoformat(created.replace("Z", "+00:00")).timestamp()
        has_gyro = any(s.get("codec_tag_string") == "djmd" for s in streams)
        out.append(Video(p, duration, start, has_gyro=has_gyro))
    return out


QUAT = re.compile(rb"t: ([-\d.e]+), v: Quaternion \{ w: ([-\d.e]+), x: ([-\d.e]+), y: ([-\d.e]+), z: ([-\d.e]+)")


def o4_body_rate(path: Path) -> np.ndarray | None:
    """Body rotation rate (rad/s, 3 axes, 2 kHz) from the O4 quaternion track."""
    if not GYRO2BB.exists():
        sys.exit(f"missing {GYRO2BB}; see README (build telemetry-parser bin/gyro2bb)")
    with tempfile.TemporaryDirectory() as tmp:
        # gyro2bb writes <input>.csv next to its input, so give it a link in tmp.
        link = Path(tmp) / "clip.mp4"
        link.symlink_to(path.resolve())
        dump = subprocess.run([str(GYRO2BB), "-d", str(link)], capture_output=True).stdout
    q = np.array(QUAT.findall(dump), dtype=float)
    if len(q) < 2000:
        return None
    t, quat = q[:, 0] / 1000.0, q[:, 1:]
    w1, x1, y1, z1 = quat[:-1].T
    w2, x2, y2, z2 = quat[1:].T
    rw = w1 * w2 + x1 * x2 + y1 * y2 + z1 * z2
    rx = w1 * x2 - x1 * w2 - y1 * z2 + z1 * y2
    ry = w1 * y2 + x1 * z2 - y1 * w2 - z1 * x2
    rz = w1 * z2 - x1 * y2 + y1 * x2 - z1 * w2
    rate = 2 * np.c_[rx, ry, rz] * np.sign(rw)[:, None] / np.median(np.diff(t))
    # Resample onto a clip-time grid starting at 0 (the first samples can precede it).
    grid = np.arange(0, t[-1], 1 / 2000.0)
    return np.stack([np.interp(grid, t[1:], rate[:, i]) for i in range(3)], 1)


# ---------- features and correlation ----------

def band_energy(x: np.ndarray, fs: int, lo: float, hi: float) -> np.ndarray:
    """Log band energy every HOP seconds, slow trend removed, standardized."""
    sos = ss.butter(4, [lo, hi], "bandpass", fs=fs, output="sos")
    y = ss.sosfiltfilt(sos, x, axis=0)
    y = (y ** 2).sum(1) if y.ndim > 1 else y ** 2
    n = int(fs * HOP)
    m = len(y) // n
    e = np.log(y[: m * n].reshape(m, n).mean(1) + 1e-9)
    if len(e) > 201:
        e = e - ss.medfilt(e, 201)
    return (e - e.mean()) / (e.std() + 1e-9)


def correlate(a: np.ndarray, v: np.ndarray, lo: float, hi: float) -> tuple[float, float, float, float] | None:
    """Best offset (video time of the audio start) within [lo, hi] s.

    Each lag is scored by its correlation over the overlap times sqrt(overlap seconds),
    since short overlaps correlate well by chance. Returns (offset, correlation, z,
    overlap s), z being the peak against the scores at every possible lag."""
    na, nv = len(a), len(v)
    full = ss.correlate(v, a, "full", method="fft")
    ones_a, ones_v = np.ones(na), np.ones(nv)
    ea = ss.correlate(ones_v, a ** 2, "full", method="fft")
    ev = ss.correlate(v ** 2, ones_a, "full", method="fft")
    overlap = np.round(ss.correlate(ones_v, ones_a, "full", method="fft")) * HOP
    lags = ss.correlation_lags(nv, na, "full") * HOP
    c = full / np.sqrt(np.maximum(ea, 1e-9) * np.maximum(ev, 1e-9))
    score = c * np.sqrt(np.maximum(overlap, 0))
    valid = overlap >= MIN_OVERLAP
    window = valid & (lags >= lo) & (lags <= hi)
    if not window.any() or valid.sum() < 20:
        return None
    null = score[valid]
    idx = np.nonzero(window)[0]
    best = idx[np.argmax(score[idx])]
    z = (score[best] - null.mean()) / (null.std() + 1e-9)
    # Parabolic refinement to a fraction of a step.
    k = lags[best]
    if 0 < best < len(score) - 1:
        y0, y1, y2 = score[best - 1], score[best], score[best + 1]
        den = y0 - 2 * y1 + y2
        if den < 0:
            k += 0.5 * (y0 - y2) / den * HOP
    return float(k), float(c[best]), float(z), float(overlap[best])


def align(a: Audio, v: Video, expected: float | None, search: float) -> Match | None:
    vf = v.feature()
    if vf is None:
        return None
    lo, hi = (-a.duration, v.duration) if expected is None else (expected - search, expected + search)
    r = correlate(a.feature(), vf, lo, hi)
    if r is None:
        return None
    off, c, z, overlap = r
    return Match(a, v, off, c, z, overlap=overlap)


def place(a: Audio, v: Video, expected: float, search: float, label: str) -> Match:
    """Offset from the clocks, refined by content when the overlap is long and the peak clear."""
    m = align(a, v, expected, search)
    if m and m.z >= Z_REFINE and m.overlap >= MIN_REFINE_OVERLAP:
        m.method = label + "+gyro"
        return m
    return Match(a, v, expected, m.corr if m else 0.0, m.z if m else 0.0, label)


def fit_group(members: list[Audio], videos: list[Video], tol: float = 1.5) -> tuple[float, int] | None:
    """Legacy power-up group: the offset that lines up recording ends with clip ends
    (both stop on disarm). Returns (power-up time, recordings matched)."""
    vends = np.array([v.start_utc + v.duration for v in videos])
    rends = np.array([m.relative_start + m.duration for m in members])
    best = None
    for re_ in rends:
        for ve in vends:
            base = ve - re_
            resid = vends[None, :] - (rends[:, None] + base)
            nearest = resid[np.arange(len(rends)), np.abs(resid).argmin(1)]
            hit = np.abs(nearest) < tol
            key = (int(hit.sum()), -float(np.abs(nearest[hit]).mean()))
            if best is None or key > best[0]:
                best = (key, base + float(np.median(nearest[hit])))
    # At least two, and at least half the group, so a partial set of clips can't
    # line up by coincidence.
    if best is None or best[0][0] < max(2, (len(members) + 1) // 2):
        return None
    return best[1], best[0][0]


# ---------- output ----------

def render(video: Video, matches: list[Match], out_dir: Path, mux: bool) -> Path:
    sr = 16000
    n = int(round(video.duration * sr))
    track = np.zeros(n, dtype=np.float32)
    for m in matches:
        pcm = m.audio.pcm()
        if m.audio.sr != sr:
            pcm = ss.resample_poly(pcm, sr, m.audio.sr).astype(np.float32)
        start = int(round(m.offset * sr))
        a0, v0 = max(0, -start), max(0, start)
        length = min(len(pcm) - a0, n - v0)
        if length > 0:
            track[v0:v0 + length] += pcm[a0:a0 + length]
    out = out_dir / f"{video.path.stem}.recorder.wav"
    wavfile.write(out, sr, np.clip(track, -32768, 32767).astype(np.int16))
    if mux:
        muxed = out_dir / f"{video.path.stem}_with_audio.MP4"
        # ffmpeg cannot write DJI's djmd/dbgi streams into MP4, so the copy has the main
        # video and the audio only; stabilize from the original clip.
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(video.path), "-i", str(out),
                        "-map", "0:v:0", "-map", "1:a", "-map_metadata", "0", "-c", "copy",
                        "-c:a", "aac", "-b:a", "192k", str(muxed)], check=True)
    return out


def fmt_time(epoch: float | None) -> str:
    if epoch is None:
        return ""
    return dt.datetime.fromtimestamp(epoch).strftime("%Y-%m-%d %H:%M:%S")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("audio_dir", type=Path, help="recorder drive (FPVAUDIO) or a copy of its WAVs")
    ap.add_argument("video_dir", type=Path, help="folder with the DJI clips")
    ap.add_argument("--out", type=Path, help="output folder (default ~/Movies/<date> FPV audio sync)")
    ap.add_argument("--search", type=float, default=120.0,
                    help="how far (s) the DJI clock may be off from GPS time (default 120)")
    ap.add_argument("--mux", action="store_true", help="also write MP4 copies with the audio added")
    ap.add_argument("--dry-run", action="store_true", help="report only, write nothing")
    args = ap.parse_args()

    audio = load_audio(args.audio_dir)
    videos = load_videos(args.video_dir)
    print(f"{len(audio)} recordings, {len(videos)} clips")
    if not audio or not videos:
        return 1

    matches: list[Match] = []

    def overlapping(a: Audio, start: float):
        """Clips that the recording overlaps, given its UTC start, with the expected offset."""
        for v in videos:
            exp = start - v.start_utc
            if exp + a.duration > 0.5 and exp < v.duration - 0.5:
                yield v, exp

    timed = [a for a in audio if a.start_utc is not None]
    if timed:
        # Session clock offset (DJI minus recorder/GPS) from long overlaps, wide search.
        cands = []
        for a in timed:
            for v in videos:
                exp = a.start_utc - v.start_utc
                if exp + a.duration > -args.search and exp < v.duration + args.search:
                    cands.append((min(a.duration, v.duration), a, v, exp))
        cands.sort(key=lambda c: -c[0])
        offsets = []
        for _, a, v, exp in cands[:10]:
            m = align(a, v, exp, args.search)
            if m and m.z >= Z_SEARCH and m.overlap >= MIN_SEARCH_OVERLAP:
                offsets.append(m.offset - exp)
        clock = 0.0
        if offsets:
            med = float(np.median(offsets))
            agree = [o for o in offsets if abs(o - med) < 1.0]
            clock = float(np.median(agree))
            print(f"Recorder clock vs DJI clock: shift recordings {clock:+.2f} s ({len(agree)} of {len(offsets)} long pairs agree)")
        else:
            print("no long pair to measure the DJI clock; using the clocks as they are")
        for a in timed:
            for v, exp in overlapping(a, a.start_utc + clock):
                matches.append(place(a, v, exp, 2.0, "time"))

    legacy = [a for a in audio if a.start_utc is None]
    groups: dict[str, list[Audio]] = {}
    for a in legacy:
        groups.setdefault(a.boot if a.relative_start is not None else f"solo:{a.path.name}", []).append(a)
    for key, members in groups.items():
        fit = fit_group(members, videos) if not key.startswith("solo:") and len(members) > 1 else None
        if fit:
            # Old firmware: one power-up's recordings placed by RECLOG.TXT uptimes.
            base, hits = fit
            print(f"  {key}: power-up at {fmt_time(base)} ({hits} of {len(members)} recording ends match clip ends)")
            for a in members:
                for v, exp in overlapping(a, base + a.relative_start):
                    matches.append(place(a, v, exp, 2.0, "log"))
            continue
        # No clock at all: search every clip; only long, clear matches are kept.
        for a in members:
            best = None
            for v in videos:
                m = align(a, v, None, 0)
                if m and m.overlap >= MIN_SEARCH_OVERLAP and (best is None or m.z > best.z):
                    best = m
            if best and best.z >= Z_SEARCH:
                best.method = "gyro search"
                matches.append(best)
            else:
                print(f"  {a.path.name}: no clear match (no clock, too short or no gyro)")

    matches.sort(key=lambda m: (m.video.start_utc, m.offset))
    print(f"\n{'clip':34} {'recording':38} {'audio at':>9} {'corr':>5} {'z':>5}  method")
    for m in matches:
        print(f"{m.video.path.name:34} {m.audio.path.name:38} {m.offset:9.2f} {m.corr:5.2f} {m.z:5.1f}  {m.method}")
    unmatched = [v.path.name for v in videos if not any(m.video is v for m in matches)]
    if unmatched:
        print(f"\nclips with no recording: {', '.join(unmatched)}")
    if args.dry_run:
        return 0

    out_dir = args.out or Path.home() / "Movies" / (
        dt.datetime.fromtimestamp(videos[0].start_utc).strftime("%Y-%m-%d") + " FPV audio sync")
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(out_dir / "sync-report.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["clip", "clip_start_local", "recording", "recording_start_local", "audio_at_clip_s",
                    "corr", "z", "method"])
        for m in matches:
            w.writerow([m.video.path.name, fmt_time(m.video.start_utc), m.audio.path.name,
                        fmt_time(m.audio.start_utc), f"{m.offset:.3f}", f"{m.corr:.3f}", f"{m.z:.2f}", m.method])
    for v in videos:
        ms = [m for m in matches if m.video is v]
        if ms:
            render(v, ms, out_dir, args.mux)
    print(f"\nwrote {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
