// 2026-10-16 V6.5 release: USB and SD openings each 0.6 mm wider.
// V6.4 case pocket, outline, seating height and flat tape-mount base retained.
// Prior fitted print bowed around the board; add 0.2 mm on each side.
// Print body flat base down and lid flat roof down. No underside rails.
// Ryan approved the V6.4 fit apart from tight port openings.
// V6.5 final port widening is CAD-verified; a new physical print is untested.
part = "exploded"; // body, lid, body_preview, fit_preview, assembly, exploded, collision
board_clearance = 0.25; // retain existing length clearance
extra_width = 0.40; // V6.4 adds 0.20 mm on each side of the board
rear_cable_width = 9; // +1 mm versus nearly fitting v4.3
rear_cable_bottom = 3; // 1 mm above the board's seating ledge
side_wire_exits = false; // Ryan requested cable exit opposite USB; keep sides solid
wall = 2.0;
floor_t = 2;
rail_h = 0; // flat continuous underside for mounting tape
pcb_l = 20.96;
pcb_w = 17.78;
pl = pcb_l + 2*board_clearance;
pw = pcb_w + 2*board_clearance + extra_width;
// Sense PCB overhangs the rear of the main PCB by 0.30 mm.
rear_extra = 0.75; // +0.4 mm pocket depth at cable end; PCB ledge unchanged
x0 = -wall-rear_extra;
x1 = pl+wall;
y0 = -wall;
y1 = pw+wall;
fpc_top = floor_t+6.93+0.25;
height_reduction = 1.0;
body_top = fpc_top+3.5-height_reduction;
lid_gap = 0.20; // internal locating lip clearance per side; not a snap fit
lid_t = 2;
seat_gap = 0; // roof underside rests on the body's top rim
lid_under = body_top+seat_gap;
lid_top = lid_under+lid_t;
lip_depth = 1.4; // lip bottom remains 1.1 mm above tallest reference connector
lip_wall = 1.2;
skirt_bottom = body_top-lip_depth;
usb_y = board_clearance+extra_width/2+2.78-((-10.59-1.65)/2);
usb_width = 12.1; // V6.5: +0.6 mm total, +0.3 mm per side
card_relief_width = usb_width+0.8; // V6.5: 12.9 mm, +0.6 mm total
card_access_depth = 8; // 3 mm deeper than V6.2; check physical access before bonding
usb_bottom = 2.1; // sill raised just 0.2 mm; below modeled connector bottom 2.26 mm
usb_top = 6.9;
wire_top = 6.6;
wire_ranges = [[pl/2+3.9,pl/2+8.8],[pl/2-8.8,pl/2-6.4]];
window_w = 10;
window_bottom = body_top-3;
eps = 0.02;
$fn = 40;

module body() {
  difference() {
    // Single flat-bottom shell; no feet or rails under the tape surface.
    translate([x0,y0,0]) cube([x1-x0,y1-y0,body_top]);
    // Full-depth pocket. Bottom board rests on the original perimeter ledge.
    translate([-rear_extra,0,floor_t])
      cube([pl+rear_extra,pw,body_top]);
    translate([1.2,1.2,floor_t-0.8]) cube([pl-2.4,pw-2.4,body_top]);
    // Top-open USB/card notch restores drop-in installation and card clearance.
    // V6.5 width 12.1 mm; actual card/cable access still needs a physical check.
    translate([pl-eps,usb_y-usb_width/2,usb_bottom])
      cube([wall+2*eps,usb_width,body_top]);
    // Top-open harness slots avoid threading soldered wires through closed holes.
    if(side_wire_exits) for(r=wire_ranges, side=[0,1])
      translate([r[0],side?pw-eps:y0-eps,floor_t+1])
        cube([r[1]-r[0],wall+2*eps,body_top]);
    // Rear foam inlet is an open-top notch capped by the lid (no TPU bridge).
    translate([x0-eps,(pw-window_w)/2,window_bottom])
      cube([wall+2*eps,window_w,body_top]);
    // Harness exit at -X, directly opposite the USB end. Open at the top so
    // the already-soldered recorder/harness can settle into the bottom.
    translate([x0-eps,(pw-rear_cable_width)/2,rear_cable_bottom])
      cube([wall+2*eps,rear_cable_width,body_top]);
  }
}

module lid() {
  difference() {
  union() {
    // Roof has exactly the body's outside width and length. Its underside
    // rests on the 2 mm body rim, leaving no overhanging outer skirt.
    translate([x0,y0,lid_under]) cube([x1-x0,y1-y0,lid_t]);
    // Small male locating lip fits inside the body pocket (a groove/rabbet
    // around its perimeter). Continuous rear edge: no wire slot in the lid.
    difference() {
      translate([-rear_extra+lid_gap,lid_gap,skirt_bottom])
        cube([pl+rear_extra-2*lid_gap,pw-2*lid_gap,lip_depth+eps]);
      translate([-rear_extra+lid_gap+lip_wall,lid_gap+lip_wall,skirt_bottom-eps])
        cube([pl+rear_extra-2*(lid_gap+lip_wall),pw-2*(lid_gap+lip_wall),lip_depth+3*eps]);
      // Remove the internal front lip above the USB/card notch.
      // The roof access cut below keeps the card accessible after bonding.
      translate([pl-lid_gap-lip_wall-0.3,usb_y-card_relief_width/2,skirt_bottom-eps])
        cube([wall+lid_gap+lip_wall+1,card_relief_width,lip_depth+3*eps]);
    }
    // Reference connector top is z=9.18; lip bottom is z=10.28. Actual
    // solder/wires and inserted card still require a physical clearance check.
  }
  // Through-roof, edge-open card access. Removes both roof and locating lip
  // over the card end; side shoulders and rear lip remain for location/bonding.
  translate([x1-card_access_depth,usb_y-card_relief_width/2,skirt_bottom-eps])
    cube([card_access_depth+eps,card_relief_width,lid_top-skirt_bottom+2*eps]);
  }
}

// Simplified visual board only; never included in either printable export.
module step_box(a,b,c,d,e,f) {
  translate([board_clearance+a+8.67,board_clearance+extra_width/2+2.78-f,floor_t+c+0.25])
    cube([b-a,f-e,d-c]);
}
module board() {
  color("seagreen") step_box(-8.67,12.28,-0.25,1,-15,2.78);
  color("seagreen") step_box(-8.97,5.70,3.93,5.18,-14.50,2.28);
  color("silver") step_box(6.51,13.81,0.01,4.21,-10.59,-1.65);
  color("silver") step_box(-5.03,5.57,1,3,-12.41,0.19);
  color("ivory") step_box(-8.84,-3.99,5.17,6.93,-12.11,0.20);
}
if(part=="body") translate([0,0,rail_h]) body();
if(part=="body_preview") color([0.25,0.45,0.7]) body();
if(part=="fit_preview") {
  color([0.25,0.45,0.7,0.55]) body();
  board();
}
if(part=="lid") translate([0,0,lid_top]) mirror([0,0,1]) lid();
if(part=="assembly" || part=="exploded") {
  color([0.25,0.45,0.7,0.7]) body();
  board();
  translate([0,0,part=="exploded"?10:0]) color([0.95,0.55,0.15,0.8]) lid();
}
if(part=="collision") {
  intersection(){ body(); lid(); }
  intersection(){ body(); board(); }
  intersection(){ lid(); board(); }
}
