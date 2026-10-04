# FPV Audio Recorder

A tiny, battery-free audio recorder for a quadcopter. The build uses a Seeed Studio XIAO ESP32S3 Sense, its microphone and microSD expansion board, and flight-controller power. The intended firmware starts a WAV when the quad arms and closes it when the quad disarms.

**Project status:** Bench recording works. The camera has been removed, and a local Easy Eject preview successfully flashed the recorder, read its temperature, and imported a WAV while leaving the microSD original intact. Flight-controller wiring and arm/disarm behavior still need hardware testing. This is not yet a flight-ready release.

The public progress page is at <https://rsmith4321.github.io/fpv-audio-recorder/>.

## What has been verified

- A 120-second mono, 16 kHz, 16-bit WAV was recorded to microSD and retrieved over USB.
- With no airflow, the ESP32-S3 internal temperature rose from 50.7°C at the start to 65.7°C when recording finished, reaching 67.7°C during the SD save. This was a bench test, not a mounted flight or long stationary test.
- The current audio level did not clip in a quiet speaker test. Propeller noise has not been recorded yet.
- After camera removal, the recorder microphone, microSD, and USB connection still worked. The imported WAV matched the original byte for byte.

## Planned first-run flow

1. Connect the XIAO Sense to a Mac using a USB data cable.
2. Easy Eject checks the board and installs a versioned recorder firmware image; the removable microSD recordings are left in place.
3. Easy Eject shows chip temperature and imports WAV files from microSD, leaving originals on the card by default.
4. On the quad, the recorder reads armed state from Betaflight over a spare UART and records while armed.

No flight-controller control commands or recorder-overheat messages are planned. Provide airflow whenever the powered quad is stationary, including USB setup.

## Mounting note

The microphone opening is on the exposed face of the Sense board beside the microSD slot. A very small piece of VHB between the boards may help retain their stack without covering that opening, provided it fits a flat, component-free gap and does not lift the board connector. Keep the microphone opening, USB port, and SD card accessible. The final tape position will be documented after the assembled board is photographed.

## Next milestones

- Test the streaming WAV firmware against real Betaflight arm/disarm status.
- Package the verified local Easy Eject flow for release, with a documented recovery path.
- Identify exact 5 V, ground, and UART pads on the arriving flight controller.
- Test the assembled quad with props off, then check audio and temperature in flight.

## Scope of this repository

This public repository holds the project page and, when ready, reviewed firmware sources and setup documentation. Bench audio, raw flash backups, personal device identifiers, and local build artifacts are intentionally excluded.
