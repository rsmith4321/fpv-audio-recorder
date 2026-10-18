// 2026-10-17 V7 SD-free recorder sleeve: one printed piece, no lid, no glue.
// Ryan: "just slide in from back no top at all to glue on, just a snug open
// back design". The XIAO + Sense stack slides in from the open back (the end
// away from USB-C) until the main board meets the front wall. A small floor
// detent at the open end and a snug side fit hold it. The roof is part of the
// sleeve and carries the mic grille and the B/R poke holes. Wires leave
// through the open back. (The zip-tie tab was removed on 2026-10-18: Ryan said
// it "just seems to get in the way". Set tie_tab = true to bring it back.)
// Board data: Seeed STEP model and Sense-board DXF (see V7-NOTES.md).
// Superseded two-part V7 draft: superseded-drafts/v7-two-part/.
part = "print"; // print, case, fit_preview, cutaway, top_view, collision

/* [Options] */
sd_socket_present = true; // false = socket desoldered: lower roof ahead of the FPC connector
side_gap = 0.25;  // per side. 0.25 = V6 width that gripped the board (snug);
                  // 0.45 = V6.4 width that dropped in freely
detent_h = 0.5;   // floor bump behind the main board; the stack lifts over it
roof_clear = 0.8; // above the tallest part that slides under each roof section

/* [Board, Seeed model] */
pcb_l = 20.96;
pcb_w = 17.78;
rear_slack = 0.3;           // room behind the main board for the detent
board_x0 = rear_slack;      // main board rear edge when pushed fully in
pl = pcb_l + rear_slack;    // 21.26, pocket length from the open end
pw = pcb_w + 2*side_gap;    // 18.28 by default

/* [Shell] */
wall = 1.6;        // front (USB) wall
side_wall = 1.2;   // 2026-10-18, Ryan: thinner sides. 3 lines with a 0.4 mm nozzle (was 1.6)
floor_t = 1.6;
roof_t = 1.6;
relief_depth = 0.8;   // centre relief under the board, as V6.x
x_rear = -0.9;        // open end, behind the detent ramp
x_front = pl + wall;
y0 = -side_wall;
y1 = pw + side_wall;

// STEP height -> case z, board resting on the floor.
function bz(h) = floor_t + h + 0.25;
// STEP x -> case x; STEP z -> case y.
function bx(x) = board_x0 + 8.67 + x;
function by(z) = side_gap + 2.78 - z;

fpc_top = bz(6.93);    // camera FPC connector (camera removed), tallest part
fpc_front = bx(-3.99); // its USB-side face, 4.98
slot_top = bz(6.77);   // microSD socket shell (dead, may be desoldered)
sense_comp_top = bz(6.19); // tallest small Sense part ahead of the FPC
sense_top = bz(5.18);
mic_top = sense_top + 0.98;
btn_top = bz(1.73);

// Roof heights. The FPC connector only travels under the rear section; the
// front section only has to pass the parts ahead of it.
roof_rear_under = fpc_top + roof_clear;
front_tallest = sd_socket_present ? max(slot_top, sense_comp_top, mic_top) : max(sense_comp_top, mic_top);
roof_front_under = max(front_tallest + roof_clear, sd_socket_present ? roof_rear_under : 0);
roof_step_x = 5.3;     // 0.3 mm ahead of the FPC connector
case_top = roof_rear_under + roof_t;

/* [USB-C] Receptacle in the Seeed model: 8.94 wide x 3.21 tall, sitting on the
   main board top (STEP height 1.00..4.21), sticking 1.53 mm past the board edge
   into the front wall. Nothing else ahead of the Sense board is taller than the
   B/R switches (1.73), so the roof drops over the USB end (Ryan, 2026-10-17). */
usb_y = by((-10.59-1.65)/2);  // receptacle centre
usb_clear = 0.25;             // per side, print tolerance
usb_width = 8.94 + 2*usb_clear;     // 9.44 window
usb_bottom = bz(1.00) - usb_clear;  // 2.60
usb_top = bz(4.21);                 // 6.06, receptacle top
roof_usb_under = usb_top + usb_clear; // 6.31, roof underside over the USB end
usb_step_x = bx(5.70) + 0.33;  // 15.0, just ahead of the Sense board's front edge
// 45-degree slope from the Sense-height roof down to the USB roof, so the part
// still prints USB face down with no supports.
usb_slope_end = usb_step_x + (roof_front_under - roof_usb_under);
// Cable overmold recess: outer 0.5 mm of the front wall, open to the top, so a
// plug's overmold can never land on the wall before the plug seats.
overmold_w = 12.4;
overmold_depth = 0.5;
overmold_bottom = 0.6;

/* [Strain relief tab, behind the floor; off since 2026-10-18] */
tie_tab = false;
tab_l = 5;
tab_w = 10;
tab_t = 1.6;
tie_slot = [2.8, 1.4];
tie_off = 3.4;

/* [Mic] Sense DXF: MIC 3.5 x 2.65 mm, centre 2.54 mm in from the D0 edge,
   7.62 mm from the main board's far end. */
mic_x = board_x0 + 7.62;
mic_y = side_gap + pcb_w - 2.54;
mic_len = [2.65, 3.5];
grille_d = 1.0;
grille_pitch = 1.3;
grille_n = [2, 3];
// No collar under the grille: the stack lifts by detent_h while sliding in, so
// a collar would have to stop above the socket anyway (V7 two-part had one).

/* [Buttons] B expected on the 5V/GND edge (-Y), R on the D0 edge (+Y). */
btn_x = bx(10.70);
btn_b_y = by(-0.10);
btn_r_y = by(-12.04);
btn_d = 2.2;
btn_sink = 3.4;

eps = 0.02;
$fn = 40;

function roof_under_at(x) = x < roof_step_x ? roof_rear_under
  : x < usb_step_x ? roof_front_under
  : x < usb_slope_end ? roof_front_under - (x - usb_step_x) : roof_usb_under;

// Side profile (x, z) of the roof underside from the open end to the front.
function ceiling_pts(xa, xb) = concat([[xb, roof_usb_under], [usb_slope_end, roof_usb_under],
  [usb_step_x, roof_front_under]],
  roof_front_under < roof_rear_under ? [[roof_step_x, roof_front_under], [roof_step_x, roof_rear_under]] : [],
  [[xa, roof_rear_under]]);
module profile_y(pts, ya, yb) // extrude an x-z polygon from y = ya to y = yb
  translate([0,yb,0]) rotate([90,0,0]) linear_extrude(yb-ya) polygon(pts);

module sleeve() {
  difference() {
    union() {
      // Outer shell: roof_t above the ceiling, low over the USB end.
      profile_y(concat([[x_rear,0],[x_front,0]],
        [for(p=ceiling_pts(x_rear,x_front)) [p[0], p[1]+roof_t]]), y0, y1);
      if(tie_tab) translate([x_rear-tab_l,(pw-tab_w)/2,0]) cube([tab_l+eps,tab_w,tab_t]);
    }
    // Tunnel, open at the rear end only.
    profile_y(concat([[x_rear-1,floor_t],[pl,floor_t]], ceiling_pts(x_rear-1,pl)), 0, pw);
    translate([1.2,1.2,floor_t-relief_depth]) cube([pl-2.4,pw-2.4,relief_depth+eps]);
    // USB-C window in the front wall, receptacle size plus usb_clear.
    translate([pl-eps,usb_y-usb_width/2,usb_bottom])
      cube([wall+2*eps,usb_width,roof_usb_under-usb_bottom]);
    // Overmold recess on the outside face.
    translate([x_front-overmold_depth,usb_y-overmold_w/2,overmold_bottom])
      cube([overmold_depth+eps,overmold_w,case_top]);
    // Tie slots.
    if(tie_tab) for(s=[-1,1])
      translate([x_rear-tab_l/2-tie_slot[0]/2,pw/2+s*tie_off-tie_slot[1]/2,-eps])
        cube([tie_slot[0],tie_slot[1],tab_t+2*eps]);
    // Mic grille.
    for(i=[0:grille_n[0]-1], j=[0:grille_n[1]-1])
      translate([mic_x+(i-(grille_n[0]-1)/2)*grille_pitch,
                 mic_y+(j-(grille_n[1]-1)/2)*grille_pitch,roof_under_at(mic_x)-0.5])
        cylinder(d=grille_d,h=case_top,$fn=24);
    // B / R poke holes with a lead-in on the outside.
    for(yy=[btn_b_y,btn_r_y]) translate([btn_x,yy,0]) {
      translate([0,0,roof_under_at(btn_x)-0.5]) cylinder(d=btn_d,h=case_top);
      translate([0,0,roof_under_at(btn_x)+roof_t-(btn_sink-btn_d)/2])
        cylinder(d1=btn_d,d2=btn_sink+0.02,h=(btn_sink-btn_d)/2+eps);
    }
  }
  // Floor detent behind the main board: 45-degree ramp from the open end,
  // square face toward the board so it can't creep back out.
  translate([0,0,floor_t-eps]) rotate([90,0,0]) translate([0,0,-pw])
    linear_extrude(pw) polygon([[x_rear,0],[x_rear+detent_h,detent_h],[board_x0-0.1,detent_h],[board_x0-0.1,0]]);
}

// Simplified board for previews and the collision check only.
module step_box(a,b,c,d,e,f) { translate([bx(a),by(f),bz(c)]) cube([b-a,f-e,d-c]); }
// tilt = rear edge raised by this much, pivoting on the main board's front
// bottom edge, as when the rear rides up the detent ramp.
// back = how far short of seated the board is while doing so.
module board(lift=0, tilt=0, back=0)
  translate([board_x0+pcb_l-back,0,floor_t]) rotate([0,atan(tilt/pcb_l),0])
  translate([-(board_x0+pcb_l),0,-floor_t+lift]) {
  color("seagreen") step_box(-8.67,12.28,-0.25,1,-15,2.78);     // XIAO
  color("seagreen") step_box(-8.97,5.70,3.93,5.18,-14.50,2.28);  // Sense
  color("silver") step_box(6.51,13.81,1.00,4.21,-10.59,-1.65);   // USB-C receptacle
  color("silver") step_box(-5.03,5.57,1,3,-12.41,0.19);          // RF shield
  color("ivory") step_box(-8.84,-3.99,5.17,6.93,-12.11,0.20);    // FPC connector
  color("lightgray") step_box(-2.21,-1.07,5.18,6.19,-9.75,-3.75); // small Sense part
  if(sd_socket_present) color("dimgray") step_box(0.30,5.70,5.18,6.77,-13.60,-0.30); // socket
  color("gold") translate([mic_x-mic_len[0]/2,mic_y-mic_len[1]/2,sense_top]) cube([mic_len[0],mic_len[1],0.98]);
  color("red") step_box(9.15,12.24,1.0,1.73,-1.0,0.6);           // B
  color("orange") step_box(9.15,12.24,1.0,1.73,-12.84,-11.24);   // R
}

if(part=="print") // front (USB) face down, open back up: no bridges, no supports
  translate([0,0,x_front]) rotate([0,90,0]) sleeve();
if(part=="case") color([0.25,0.45,0.7]) sleeve();
if(part=="fit_preview") { color([0.25,0.45,0.7,0.5]) sleeve(); board(); }
if(part=="cutaway") {
  color([0.25,0.45,0.7]) difference(){ sleeve(); translate([x_rear-10,-10,-1]) cube([60,usb_y+10,30]); }
  board();
}
if(part=="top_view") {
  board();
  color("black") difference() {
    translate([x_rear,y0,case_top]) cube([x_front-x_rear,y1-y0,0.1]);
    translate([x_rear+0.3,y0+0.3,case_top-1]) cube([x_front-x_rear-0.6,y1-y0-0.6,2]);
  }
  color("blue") for(i=[0:grille_n[0]-1], j=[0:grille_n[1]-1])
    translate([mic_x+(i-(grille_n[0]-1)/2)*grille_pitch,mic_y+(j-(grille_n[1]-1)/2)*grille_pitch,mic_top])
      cylinder(d=grille_d,h=case_top-mic_top,$fn=24);
  color("blue") for(yy=[btn_b_y,btn_r_y]) translate([btn_x,yy,btn_top])
    difference(){ cylinder(d=btn_d,h=case_top-btn_top); translate([0,0,-1]) cylinder(d=btn_d-0.4,h=20); }
}
if(part=="collision") {
  intersection(){ sleeve(); board(); }       // seated
  // Riding over the detent: the rear tips up by detent_h on the front edge.
  // (A straight 0.5 mm lift would no longer pass: the USB roof is 0.25 mm over
  // the receptacle.) Each part only ever slides under roof it later sits under.
  // The rear edge is at the top of the ramp, 0.7 mm short of seated.
  intersection(){ sleeve(); board(tilt=detent_h, back=board_x0-(x_rear+detent_h)); }
}
