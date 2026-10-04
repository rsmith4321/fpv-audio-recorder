# FPV Audio Recorder

A tiny, battery-free audio recorder for a quadcopter. The build uses a Seeed Studio XIAO ESP32S3 Sense, its microphone and microSD expansion board, and flight-controller power. The intended firmware starts a WAV when the quad arms and closes it when the quad disarms.

**Project status:** Bench recording works. The flight firmware, Easy Eject setup/import flow, and flight-controller wiring are in progress. This is not yet a flight-ready release.

The public progress page is at <https://rsmith4321.github.io/fpv-audio-recorder/>.

## What has been verified

- A 120-second mono, 16 kHz, 16-bit WAV was recorded to microSD and retrieved over USB.
- With no airflow, the ESP32-S3 internal temperature rose from 50.7°C at the start to 65.7°C when recording finished, reaching 67.7°C during the SD save. This was a bench test, not a mounted flight or long stationary test.
- The current audio level did not clip in a quiet speaker test. Propeller noise has not been recorded yet.

## Planned first-run flow

1. Connect the XIAO Sense to a Mac using a USB data cable.
2. Easy Eject identifies the board and installs a versioned recorder firmware image after checking the board and preserving existing recordings.
3. Easy Eject shows chip temperature and imports WAV files from microSD, leaving originals on the card by default.
4. On the quad, the recorder reads armed state from Betaflight over a spare UART and records while armed.

No flight-controller control commands or recorder-overheat messages are planned. Provide airflow whenever the powered quad is stationary, including USB setup.

## Next milestones

- Finish and verify streaming WAV firmware with arm/disarm status input.
- Package and verify the one-click flash image and recovery path.
- Complete Easy Eject recorder connection, temperature monitor, and safe import.
- Identify exact 5 V, ground, and UART pads on the arriving flight controller.
- Test the assembled quad with props off, then check audio and temperature in flight.

## Scope of this repository

This public repository holds the project page and, when ready, reviewed firmware sources and setup documentation. Bench audio, raw flash backups, personal device identifiers, and local build artifacts are intentionally excluded.
