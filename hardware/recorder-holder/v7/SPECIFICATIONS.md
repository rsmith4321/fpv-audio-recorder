# V7 sleeve specifications

Dimensions are in millimetres. CAD values describe nominal geometry, not measurements of a printed TPU part.

| Feature | Nominal dimension |
| --- | --- |
| Outside length × width | 23.76 × 20.68 |
| Height at the rear (over the Sense board) | 11.18 |
| Height over the USB end | 7.91 |
| Mass in TPU 95A (1.21 g/cm³) | 2.15 g (socket-removed variant 2.12 g) |
| Mass in PETG (1.27 g/cm³) | 2.26 g |
| Volume | 1777.8 mm³ |
| Main PCB reference length × width | 20.96 × 17.78 |
| Pocket length × width | 21.26 × 18.28 |
| Side clearance (`side_gap`) | 0.25 per side |
| Rear slack behind the main board | 0.30 |
| Side walls | 1.2 |
| Front (USB) wall, floor, roof | 1.6 |
| Floor centre relief under the board | 0.8 deep |
| Floor detent (`detent_h`) | 0.5 high, 45° ramp from the open end, square face toward the board |
| Roof clearance over the tallest part under it | 0.8 |
| USB-C receptacle (Seeed model) | 8.94 × 3.21 |
| USB window (`usb_clear` 0.25 all round) | 9.44 × 3.71, bottom 2.60 and top 6.31 above the case bottom |
| Overmold recess on the front face | 12.4 wide, 0.5 deep, from 0.6 up, open to the top |
| Mic grille | 2 × 3 holes, 1.0 diameter, 1.3 pitch, centred 7.62 from the open-end board edge and 2.54 in from the D0 edge |
| B / R poke holes | 2.2 diameter, 3.4 countersink, 4.3 deep from the top to the switch |
| Print footprint as saved (standing) | 11.18 × 20.68 on the plate, 23.76 tall |

Board positions come from Seeed's STEP model and the Sense-board DXF, not from measurements. The optional camera is removed in this build. The tallest part under the rear roof is the camera FPC connector.

## Socket-removed variant

`sd_socket_present = false` lowers the front 17.5 mm of roof by 0.74 mm (10.44 instead of 11.18 there). The overall height doesn't change, because the FPC connector at the rear sets it. It saves about 0.04 g, so print the default unless the socket is already off.

## Rebuilding the meshes

With OpenSCAD installed:

```sh
openscad --export-format binstl -o recorder-case-v7-sleeve.stl recorder-case-v7.scad
openscad --export-format binstl -D sd_socket_present=false -o recorder-case-v7-sleeve-socket-removed.stl recorder-case-v7.scad
```

The default `part = "print"` is already oriented for printing (USB face down). Other views: `case`, `fit_preview`, `cutaway`, `top_view`, `collision`. Useful options: `side_gap`, `detent_h`, `usb_clear`, `overmold_depth`, `tie_tab`.

## Verification

- Both STLs and the 3MF are closed and manifold, one connected part each, with no orientation errors (Euler characteristic −16, matching the holes). Results in [mesh-validation.json](mesh-validation.json).
- Re-exporting both variants from `recorder-case-v7.scad` gives the same triangle count, volume and size as the published meshes.
- Collision check against a simplified board model built from the Seeed STEP data, seated and while riding over the detent: the only contact is where the board rests on the floor.
- Physical: the default sleeve was printed in TPU on 2026-10-18. The board slides in snugly, the USB port lines up, and the case is VHB-mounted on a Flywoo Firefly 25 Mini.
- The installed recorder flew in the V7 sleeve on 2026-10-22 and retained onboard audio; see the [field-test report](../../../docs/flight-test-2026-10-22.md).
- **Remaining evaluation:** repeated-flight retention, measured vibration, wind-noise protection through the grille, and temperature in flight.
