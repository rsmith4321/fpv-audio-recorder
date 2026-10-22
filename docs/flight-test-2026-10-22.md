# Real-flight test: firmware v2 and V7 case, 22 October 2026

The core DIY build and first real-flight milestone are complete: the published firmware v2 flew in the printed V7 TPU case and retained 110.18 seconds of onboard audio with zero missing encoded frames.

[Watch the flight video with onboard audio](https://rsmith4321.github.io/fpv-audio-recorder/#flight-test) or [download the MP4](media/flight-test-2026-10-22.mp4).

## The video sample

The sample is an **87.00-second selected excerpt**, stabilized and color-graded in GyroGrade. It uses the matching portion of the main recorder track, without music or a gain adjustment. The public MP4 is 720p with AAC audio; the original 4K camera footage and recorder WAV are preserved separately.

The original DJI container clock was about an hour ahead of the recorder's clock. Matching prop-noise changes to the original O4 gyro data placed the recorder's start approximately 0.63 seconds before the original video. Source/export frame comparisons were used to check the selected trim, so the sample's sound starts about 12.0 seconds into the main recording. The alignment is an estimate, not an independent timing calibration.

## Retained recordings

| Recording | Decoded duration | Format | Decoded peak | Missing encoded frames |
|---|---:|---|---:|---:|
| Short recording | 2.82 s | 16 kHz mono, signed 16-bit WAV | −30.54 dBFS | 0 |
| Main recording | 110.18 s | 16 kHz mono, signed 16-bit WAV | −7.64 dBFS | 0 |

Both WAV durations match `FLIGHTS.CSV`. Both entries report a `DISARM` ending, the flight controller's clock (`FC`), and `start_overwritten=no`. Raw recorder metadata reports intact starts, and separately retrieved Opus streams report zero missing encoded frames. Independent Opus decodes agree closely with the USB-drive WAVs after accounting for decoder pre-skip.

These are recording durations and recorder metadata. They are not independent measurements of airborne time or clock accuracy. The short recording should not be read as evidence of a second full flight.

## What the audio establishes

The main recording contains sustained, changing harmonic sound consistent with motor and propeller noise during the reported flight. Neither decoded file has full-scale rail samples or an all-zero 20 ms block. The loss count comes from raw encoded-frame retrieval, rather than an inference from the waveform.

The main file's decoded peak leaves 7.64 dB of digital headroom. That does **not** prove the microphone avoided overload: the capture path applies about −12 dB of digital attenuation after receiving microphone samples, and lossy Opus decoding also changes peaks. Lowering that digital gain cannot repair distortion introduced before it.

The sample is functional onboard sound at 16 kHz, representing frequencies up to 8 kHz. Audio level and spectra alone cannot separate wind, prop wash, vibration and motor sound, or establish windscreen effectiveness. No in-flight temperature measurement was made. One flight also does not establish long-term case retention or recorder reliability.

## Remaining work

USB sometimes needs a recorder reset before the drive appears. A local startup fix was installed after this flight; it is not included in the published v2 source or binary and has not been flown. Repeatability testing remains unfinished after a harness wire needed resoldering. Repeated cold starts, reconnects, late USB insertion while externally powered and flight-controller-triggered recording under that candidate still need validation.

Easy Eject does not yet support the v2 USB device. Firmware v2 and the V7 case remain pre-release downloads, and the automatic-update manifest stays on its previous version. The real-flight result completes the core DIY build milestone; it does not establish production-grade reliability or validate the later USB candidate.
