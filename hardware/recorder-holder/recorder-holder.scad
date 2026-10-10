// FPV audio recorder holder, v2 (2026-10-10). Not yet printed or test-fitted.
// Holds the bottom board of a Seeed XIAO ESP32S3 Sense stack (camera removed) by its edges.
// Dimensions come from Seeed's official 3D model (seeed-studio-xiao-esp32s3-sense-3d_model.zip):
//   main board 20.96 x 17.78 mm, 1.25 mm thick, flat underside;
//   Sense board sits 2.9 mm above it and does not cover the USB-C end;
//   USB-C overhangs the board end by 1.5 mm.
// The walls stop below the main board's top surface, so they never touch the harness
// wires, which leave from the gap between the two boards. The top stays fully open.
// Orientation: length along X with the USB-C end at +X; width along Y; up is +Z.
// Units: mm.

/* [Board, from Seeed's model] */
board_len = 20.96;
board_wid = 17.78;
board_t   = 1.25;

/* [Fit] */
clearance  = 0.15;  // per side; raise to 0.25 if the board will not drop in
wall_below_top = 0.25; // wall top sits this far below the board's top surface

/* [Holder] */
ledge_t   = 2.0;    // base thickness under the board's edges
ledge_w   = 1.2;    // width of the ledge the board edges rest on
tape_recess = 0.8;  // pocket under the board for a VHB square (1.0-1.1 mm VHB compresses into it)
wall_t    = 1.2;
rail_h    = 1.5;    // underside rails = air gap above the O4's metal top
rail_w    = 3.0;
rail_count = 3;
// Wall notches at the solder pads used by the harness, as [start, end] along X measured
// from the board's centre. Pins are on a 2.54 mm pitch, 7.62 mm either side of centre:
// 5V and GND are the two pins nearest the USB end; D6 and D7 are the last pins at the
// far end. Both sides are notched so either solder position clears.
notch_ranges = [[3.9, 8.8], [-8.8, -6.4]];

$fn = 48;
pl = board_len + 2*clearance;
pw = board_wid + 2*clearance;
wall_h = board_t - wall_below_top;   // wall height above the ledge

module holder() {
  difference() {
    union() {
      // base
      translate([-wall_t, -wall_t, 0]) cube([pl + wall_t, pw + 2*wall_t, ledge_t]);
      // walls on three sides; the USB-C end (+X) stays open
      translate([-wall_t, -wall_t, ledge_t]) cube([wall_t, pw + 2*wall_t, wall_h]);
      translate([-wall_t, -wall_t, ledge_t]) cube([pl + wall_t, wall_t, wall_h]);
      translate([-wall_t, pw, ledge_t]) cube([pl + wall_t, wall_t, wall_h]);
      // underside rails, lengthwise so air passes under
      for (i = [0 : rail_count - 1])
        translate([-wall_t, -wall_t + (pw + 2*wall_t - rail_w) * i / (rail_count - 1), -rail_h])
          cube([pl + wall_t, rail_w, rail_h]);
    }
    // VHB recess inside the ledge
    translate([ledge_w, ledge_w, ledge_t - tape_recess])
      cube([pl - 2*ledge_w, pw - 2*ledge_w, tape_recess + 0.01]);
    // notches at the harness pads
    for (r = notch_ranges, side = [0, 1])
      translate([pl/2 + r[0], side ? pw - 0.01 : -wall_t - 0.01, ledge_t])
        cube([r[1] - r[0], wall_t + 0.02, wall_h + 0.01]);
  }
}

holder();

// Preview only: simplified board, using measurements from Seeed's 3D model
// (camera removed). It is never part of the exported part.
show_board = true;
// STEP (x, y, z) box -> holder coordinates: x + 8.67, 2.78 - z, y + 0.25, plus clearance.
module step_box(x0, x1, y0, y1, z0, z1)
  translate([clearance + x0 + 8.67, clearance + 2.78 - z1, ledge_t + y0 + 0.25])
    cube([x1 - x0, z1 - z0, y1 - y0]);
module board() {
  color([0.12, 0.40, 0.22]) step_box(-8.67, 12.28, -0.25, 1.00, -15.00, 2.78);  // XIAO main board
  color([0.12, 0.40, 0.22]) step_box(-8.97, 5.70, 3.93, 5.18, -14.50, 2.28);    // Sense board
  color([0.75, 0.75, 0.78]) step_box(6.51, 13.81, 0.01, 4.21, -10.59, -1.65);   // USB-C
  color([0.75, 0.75, 0.78]) step_box(-5.03, 5.57, 1.00, 3.00, -12.41, 0.19);    // RF shield
  color([0.92, 0.92, 0.88]) step_box(-8.84, -3.99, 5.17, 6.93, -12.11, 0.20);   // camera FPC connector
}
if (show_board) board();
