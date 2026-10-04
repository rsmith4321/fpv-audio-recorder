# Recorder firmware

`latest.json` is the small version manifest that the experimental Easy Eject recorder mode checks on GitHub. Version `0.1.0` is a **bench candidate** for the 8 MB Seeed Studio XIAO ESP32S3 Sense. The source and merged flash image are included here with a SHA-256 digest.

The image was flashed and tested locally for microphone capture, microSD WAV writing, USB temperature reporting, file listing, and byte-matched WAV transfer. It has **not** been tested with a real flight controller's arm/disarm state, in flight, or for power loss during recording. Use it for bench setup only until those checks are complete. The removable microSD card is separate from the ESP32 internal flash image.

The serial identity string in this first build does not include a separate firmware version; Easy Eject maps its exact `RECORDER 1 XIAO_ESP32S3_SENSE 16000 MONO16` identity to `0.1.0`. Later versions will report their version explicitly.
