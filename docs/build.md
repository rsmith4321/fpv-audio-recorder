# Prototype build: camera removal and recorder wires

**Updated October 22, 2026.** These photographs show the early camera-removal and wiring stages. They are cropped and lightly sharpened for readability; solder joints and components have not been retouched. The completed build now uses SD-free [firmware v2](../firmware/v2/README.md) and the [V7 TPU case](../hardware/recorder-holder/v7/README.md), with a [real-flight result](flight-test-2026-10-22.md). See the [Flywoo connector installation](flywoo-goku-f405-se.md) for the assembled harness and mounting history.

## Hardware

- Seeed Studio XIAO ESP32S3 **Sense**, including its microphone/microSD expansion board.
- A microSD card was used for the early firmware pictured here. Current firmware v2 uses internal flash and needs no card.
- Four lightweight insulated wires; this prototype uses roughly 6–7 inch lengths, to be trimmed for the quad.
- One 1N5817 Schottky diode for the regulated power lead, solder and heat-shrink tubing. The Flywoo prototype uses its labeled 4.5 V socket; see the controller-specific guide.
- 3M VHB mounting tape; the current build also uses the V7 printed sleeve.
- USB data cable for setup. No separate recorder battery.

The [Seeed pin map and power instructions](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) are the reference for VBUS/5V, ground, D6/TX and D7/RX. Flight-controller pad names and supply capacity must be checked for the specific controller before connecting it.

## 1. Original stack

The camera arrives attached to the Sense expansion board. The microphone and microSD slot are on that expansion board, so the expansion board stays in the recorder.

![Original XIAO Sense stack with camera](../images/01-original-stack.jpg)

The side view shows the two-board construction and connector gap.

![Side view before camera removal](../images/02-board-gap.jpg)

## 2. Separate the boards and remove the camera

With USB and all power disconnected, gently separate the two boards at their board-to-board connector. Support the boards rather than pulling on the camera ribbon.

![Sense expansion board and main XIAO separated](../images/03-separated-boards.jpg)

Carefully release the camera connector's small dark locking flap, then slide out the camera ribbon. Keep the microphone and SD board intact. In the following photograph the removed camera is on the left, the Sense expansion board is in the middle, and the main XIAO is on the right.

![Camera removed with the microphone and microSD expansion retained](../images/04-camera-removed.jpg)

The microphone is the small rectangular package with a circular opening beside the microSD holder. Its opening must stay unobstructed.

## 3. Solder four wires to the main XIAO

This prototype solders to the inward face of the plated edge pads, with the wires exiting sideways. Keep joints low enough that the Sense board can fully seat without touching solder or exposed wire. No pin headers are fitted.

| Wire in this prototype | XIAO pad | Intended flight-controller connection |
| --- | --- | --- |
| Red | VBUS (called 5V in Seeed's pin map) | Regulated 5 V **through the diode** |
| Black | GND | Ground |
| Purple | D6 / TX / GPIO43 | Spare UART RX |
| Yellow | D7 / RX / GPIO44 | The same UART's TX |

In this photograph, USB is at the top: red and black are at the upper right; purple is at the lower left and yellow at the lower right. Always verify pad labels and the official pin map; a flipped board reverses the apparent sides.

![Four wires soldered to the main XIAO](../images/05-soldered-wires.jpg)

The photo records the prototype joints. Check for bridges, loose strands and clearance, and check the unpowered power wiring for a short before reconnecting USB. A photograph cannot establish electrical continuity.

## 4. Reassemble and retain the stack

Align and fully seat the board-to-board connector without forcing it. The microphone is on the exposed face beside the SD slot, away from the gap between boards.

![Camera-free audio recorder reassembled with four wires](../images/06-audio-recorder-stack.jpg)

After solder inspection and camera removal, use a very small VHB patch in a flat, component-free part of the gap to help secure the boards together. The tape must fit without lifting the connector or pressing on components. The exact tape placement and crash retention still need testing. Keep USB, the SD card and the microphone opening accessible.

## 5. Diode, flight-controller wiring and testing

The diode is not shown installed in these photographs. Fit **one** diode inline in the red lead: the unstriped anode goes toward the flight controller's regulated 5 V supply; the striped cathode goes toward the XIAO VBUS pad. Insulate the diode and both joints with heat shrink. Never connect VBUS directly to quad battery voltage. Seeed specifies a series diode for external power into this pad.

For the Flywoo GOKU F405 SE prototype, the [controller-specific installation guide](flywoo-goku-f405-se.md) uses its UART1 five-position socket and a supplied four-wire harness, avoiding soldering onto the flight controller. That socket is labeled 4.5 V; no supply or VBUS measurement under load is documented. The harness, diode and MSP configuration were completed on October 10, 2026, and the recorder ran from that socket through the diode with its USB unplugged, starting on arm and closing the file on disarm (see the installation guide for photos and results). For other controllers, identify the exact regulated supply, ground and spare UART before connecting.

Provide airflow while the powered quad is stationary, including computer setup. The two-minute still-air bench result is an observation, not a long-idle thermal rating. The October 22 flight demonstrated onboard recording with the V7 case. In-flight temperature, measured vibration, repeated-flight retention and power-loss behavior still need separate validation.

## Firmware and getting the audio

Current [firmware v2](../firmware/v2/README.md) records Opus audio to internal flash and presents a read-only `FPVAUDIO` USB drive with one timestamped WAV per flight and `FLIGHTS.CSV`. Copy the WAVs from that drive; no microSD card is needed. The [host tools](../firmware/v2/host-tools/) can also retrieve recordings over USB serial and estimate alignment with DJI clips.

The [earlier microSD firmware](../firmware/README.md) and its local Easy Eject flash/import tests are retained as project history. Easy Eject does not yet recognize v2, so use the v2 installation and transfer instructions. USB startup sometimes needs a recorder reset; the [field report](flight-test-2026-10-22.md#remaining-work) describes the remaining work. See also the [Easy Eject project help page](https://easyeject.com/help/fpv-audio-recorder).
