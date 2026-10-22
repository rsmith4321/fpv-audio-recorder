# Recorder firmware

## Current: firmware v2 (pre-release)

[Firmware v2](v2/README.md) records to the XIAO ESP32S3's internal flash instead of microSD and shows up on a computer as a read-only USB drive, `FPVAUDIO`, with one WAV per flight, timestamped from the flight controller's clock. About 30 minutes fit, and the oldest audio is overwritten when it's full. **Flown in the V7 case on 2026-10-22**, with 110.18 s and 2.82 s recordings closed on disarm, flight-controller timestamps and zero missing encoded frames. See the [flight-test report](../docs/flight-test-2026-10-22.md). USB startup reliability remains under investigation; a newer local candidate was installed after the flight and is not included in this release. Source, a flashable image, host tools and a host test are in [`v2/`](v2/).

## Previous: v0.1.0 bench candidate (microSD)

`latest.json` is the small version manifest that the experimental Easy Eject recorder mode checks on GitHub. Version `0.1.0` is a **bench candidate** for the 8 MB Seeed Studio XIAO ESP32S3 Sense. The source and merged flash image are included here with a SHA-256 digest.

The image was flashed and tested locally for microphone capture, microSD WAV writing, USB temperature reporting, file listing, and byte-matched WAV transfer. It has **not** been tested with a real flight controller's arm/disarm state, in flight, or for power loss during recording. Use it for bench setup only until those checks are complete. The removable microSD card is separate from the ESP32 internal flash image.

The serial identity string in this first build does not include a separate firmware version; Easy Eject maps its exact `RECORDER 1 XIAO_ESP32S3_SENSE 16000 MONO16` identity to `0.1.0`. Later versions will report their version explicitly.

`latest.json` still points at v0.1.0, because Easy Eject's recorder mode doesn't recognise the v2 USB device yet.
