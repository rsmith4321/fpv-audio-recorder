# FPV Audio Recorder

Onboard FPV audio without carrying an extra GoPro or other action camera. This work in progress is a tiny, battery-free audio recorder for a quadcopter. The build uses a Seeed Studio XIAO ESP32S3 Sense with its microphone board, powered from the flight controller. The firmware starts a recording when the quad arms and closes it when the quad disarms.

**Project status:** Pre-release, bench-tested 2026-10-18; flight test pending. [Firmware v2](firmware/v2/README.md) records to the XIAO's internal flash (no microSD) and shows up on a computer as a read-only USB drive with one timestamped WAV per flight. It was flashed and tested on the bench over USB. The [V7 one-piece case](hardware/recorder-holder/README.md) has been printed in TPU, fitted and mounted on the drone. Earlier, on October 10, 2026, the earlier microSD firmware recorded on Betaflight arm/disarm with the harness and diode installed (props-off and indoor hand-held tests), and the first real-flight recordings followed on October 13. Firmware v2 has not yet recorded a flight or a live arm/disarm, and the V7 case hasn't been flown. This is not yet a flight-proven release.

The public progress page is at <https://rsmith4321.github.io/fpv-audio-recorder/>.

![Camera-free recorder prototype](images/06-audio-recorder-stack.jpg)

Follow the [photographed prototype build](docs/build.md) for camera removal, the four recorder wires, and the planned diode connection. For the Flywoo build, see the [connector installation plan](docs/flywoo-goku-f405-se.md): no soldering onto the flight controller, using its UART1 socket and supplied harness. **Next: flight-test firmware v2 in the V7 case.**

## What has been verified

Firmware v2 (bench, 2026-10-18, USB power):

- The recorder mounts in macOS as the read-only `FPVAUDIO` drive, and recordings from the earlier firmware survived the update.
- Audio copied from the drive matches the serial download; copying runs at 9–10× real time.
- A bench-clock recording got the right timestamped name and matching times in `FLIGHTS.CSV` and the WAV's time reference, with 0 lost frames. Chip temperature stayed near 40°C without airflow, well below the SD firmware's 50–68°C.
- The B-button format erased everything without a restart.

Earlier microSD firmware (v0.1.0, then a −12 dB gain build that wasn't published separately):

- Recorded on Betaflight arm/disarm from the flight controller's 4.5 V socket through the diode (October 10), then 16 outdoor flight recordings on October 13 with every file closed cleanly and no clipped samples.

## Using it

1. Fly: the recorder reads armed state from Betaflight over a spare UART (read-only) and records while armed. It never sends flight commands.
2. Plug the recorder into a computer with a USB-C data cable and copy the WAVs off the `FPVAUDIO` drive.
3. Optionally run `audio_video_sync.py` to line the audio up with DJI O4 Pro clips.

Provide airflow whenever the powered quad is stationary, including USB setup.

## Mounting note

The microphone opening is on the exposed face of the Sense board. The four wires are soldered to the inward face of the main XIAO's plated edge pads, with low-profile joints protected inside the gap and wires exiting sideways. Check that the board connector seats fully and no solder touches components on the Sense board. In the V7 case the stack slides in from the open back and the wires leave through it; secure the harness to the frame, and keep glue well away from the microphone.

## Firmware v2: the recorder as a USB drive

[Firmware v2](firmware/v2/README.md) (pre-release, bench-tested 2026-10-18; flight test pending):

- Plug the recorder into a computer and it appears as a read-only USB drive, `FPVAUDIO`, with **one WAV per flight**, `FLIGHTS.CSV` and a `README.TXT`.
- Files are named by the flight controller's clock, e.g. `FPV_20261018173825_P0005_F01.WAV`, and carry a Broadcast WAV time reference so editors can place them by time of day.
- About 30 minutes fit in the internal flash; the **oldest audio is overwritten** when it's full.
- **Format:** hold the button marked **B** for about 10 seconds while steadying the plug; the LED blinks, blinks faster, then gives **three long flashes** when everything is erased.
- Host tools download over USB serial and line the audio up with DJI O4 Pro clips.

## 3D-printed case

The [V7 one-piece TPU sleeve](hardware/recorder-holder/README.md) (23.76 × 20.68 × 11.18 mm, 2.15 g) holds the SD-free recorder, which slides in from the back with no lid or glue. It has a mic grille and B/R button holes. Print it standing, USB face down, as saved, and dry the TPU first. **Bench-tested 2026-10-18; flight test pending.** The previous [V6.5 two-piece case](hardware/recorder-holder/v6.5/README.md) remains for microSD builds. Alternative designs and improvements are welcome through pull requests or issues.

![V7 case on the drone](hardware/recorder-holder/v7/photos/v7-on-drone.jpg)

## Next milestones

- Flight-test firmware v2 and the V7 case: arm/disarm with the flight controller's clock, audio level, wind noise, retention and temperature.
- Update the Easy Eject recorder mode to find the v2 USB device, then move `firmware/latest.json` to v2.
- Package the verified local Easy Eject flow for release, with a documented recovery path.

## Scope of this repository

This public repository holds the project page, the case designs, and the versioned firmware source and images. Bench audio, personal recordings, raw flash backups, personal device identifiers, and unrelated local build artifacts are intentionally excluded.

## Related work

[FPVSoundLogger](https://github.com/SebGalina/FPVSoundLogger) is another open-source approach to onboard FPV audio recording. This project explores a XIAO Sense with its integrated microphone, recording to internal flash and appearing as a USB drive.
