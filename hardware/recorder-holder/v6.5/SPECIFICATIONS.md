# V6.5 case specifications

Dimensions are in millimetres. CAD values describe nominal geometry, not measurements of a printed TPU part.

| Feature | Nominal dimension |
| --- | --- |
| Outside length × width | 26.21 × 22.68 |
| Bottom-case height | 11.68 |
| Closed outside height, lid seated | 13.68 |
| Lid roof thickness | 2.00 |
| Lid total height including locating lip | 3.40 |
| Main wall thickness | 2.00 |
| Board seating ledge above flat underside | 2.00 |
| Floor thickness below central recessed area | 1.20 |
| Main PCB reference length × width | 20.96 × 17.78 |
| Pocket length including rear allowance | 22.21 |
| Pocket width | 18.68 |
| PCB clearance along length | 0.25 per end, plus 0.75 rear pocket allowance |
| PCB clearance across width | 0.45 per side |
| Internal lid locating lip depth | 1.40 |
| Internal lid locating lip wall thickness | 1.20 |
| Lid-to-pocket clearance | 0.20 per side |
| Top-open body USB/card opening width | 12.10 |
| Body USB opening sill above underside | 2.10 |
| Lid SD access width × inward depth | 12.90 × 8.00 |
| Opposite-end harness notch width | 9.00 |
| Harness notch sill above underside | 3.00 |
| Upper rear inlet notch width | 10.00 |
| Flat underside tape area | 26.21 × 22.68, about 594.44 mm² |

The board envelope is derived from the locally saved Seeed reference model; solder joints, wire routing, card projection, and USB cable shells need physical fit checks. The optional camera is removed in this build.

V6.4 added 0.20 mm on each side of the board and removed the 1.5 mm underside rails without lowering the internal seating ledge. The builder approved that fitted case apart from tight port access. V6.5 widens only the body USB/card opening and the lid SD opening by 0.60 mm overall each (0.30 mm per side), keeping them centred in the same positions. Their depths and sill heights, the pocket, outside dimensions, and lid locating fit are unchanged.

## Rebuilding the meshes

With OpenSCAD installed, export each part separately:

```sh
openscad -D 'part="body"' -o recorder-case-v6.5-bottom-flat.stl recorder-case-v6.5.scad
openscad -D 'part="lid"' -o recorder-case-v6.5-lid.stl recorder-case-v6.5.scad
```

Other source views include `body_preview`, `fit_preview`, `assembly`, `exploded`, and `collision`. These are inspection views; use `body` and `lid` for printable exports.

## Verification

Both exported STLs were checked for closed edges and a single connected surface. Their footprints match at 26.21 × 22.68 mm. The bottom mesh has a continuous rectangular underside with no feet or rails. The previews were reviewed visually. The printed V6.4 fit was approved by the builder apart from tight port access. V6.5's final wider openings have not been physically reprinted. Lid retention, mounting-tape strength, thermal behaviour, and wind-noise performance remain unverified.
