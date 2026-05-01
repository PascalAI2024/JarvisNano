// =============================================================================
// J.A.R.V.I.S. Enclosure — Concept 1: Monolith
// Matte charcoal rectangular slab, single horizontal LED slit above round display cutout.
// Stacked sandwich layout: battery bottom, amp+speaker mid, XIAO top.
// Lid = top panel, secured with 4x M2 brass inserts + M2 cap-head screws.
// =============================================================================

// --- Global parameters -------------------------------------------------------
WALL       = 2.0;   // Wall thickness mm
FN_HI      = 64;    // Circle facets high-quality
FN_LO      = 32;    // Circle facets standard

// --- Outer envelope ----------------------------------------------------------
OW = 78.0;   // Outer width  (X)
OD = 68.0;   // Outer depth  (Y)
OH = 66.0;   // Outer height (Z)

// --- Inner cavity ------------------------------------------------------------
IW = OW - 2*WALL;   // 74.0 mm
ID = OD - 2*WALL;   // 64.0 mm
IH = OH - 2*WALL;   // 62.0 mm

// --- Component dimensions ----------------------------------------------------
// XIAO ESP32-S3 Sense
XIAO_W = 21.0;
XIAO_D = 17.5;
XIAO_H = 3.5;
XIAO_USB_SIDE = "front";  // USB-C faces front wall

// Acoustic mic port (PDM mic is ~3 mm from USB-C edge, i.e. front wall)
MIC_PORT_D = 1.8;   // diameter mm (within 1.5-2 mm spec)

// Speaker + amp module (Option A — PAM8002A, larger case)
SPK_W = 50.0;
SPK_D = 30.0;
SPK_H = 18.0;   // includes dome

// Speaker dome diameter (28 mm speaker)
SPK_DOME_D = 28.0;

// Speaker grille hole diameter and pitch
GRL_D    = 2.0;
GRL_PITCH = 3.5;

// LiPo battery cavity (fits both 503040 and 503450)
BAT_W = 52.0;
BAT_D = 35.0;
BAT_H = 6.0;    // clearance for thicker 503450 (5 mm) + foam

// Round display cutout (GC9A01 — Phase 1: blank plate; Phase 3: screen)
DISP_D_OUTER = 39.0;   // outer diameter of cutout
DISP_D_PLATE = 37.0;   // blank plate OD (2 mm lip each side)

// LED slit
LED_W = 40.0;
LED_H = 2.5;
LED_Y_FROM_TOP = 10.0;  // distance from top of front face

// M2 boss geometry
BOSS_OD = 6.0;
BOSS_H  = 8.0;
M2_DRILL = 2.2;   // clearance drill through lid
M2_INSERT_D = 3.2; // brass heat-set insert OD

// Lid thickness
LID_T = 2.5;

// Corner fillet radius (subtle — Monolith stays angular)
CR = 3.0;

// =============================================================================
// Utility modules
// =============================================================================

module rounded_box(w, d, h, r) {
    // Minkowski with small sphere for subtle edge rounding
    minkowski() {
        cube([w - 2*r, d - 2*r, h - 2*r], center=true);
        sphere(r=r, $fn=FN_LO);
    }
}

module speaker_grille(rows, cols, pitch, hole_d) {
    // Grid of circular holes for speaker grille
    for (row = [0 : rows-1]) {
        for (col = [0 : cols-1]) {
            x = (col - (cols-1)/2) * pitch;
            y = (row - (rows-1)/2) * pitch;
            translate([x, y, 0])
                cylinder(d=hole_d, h=WALL*3, center=true, $fn=FN_LO);
        }
    }
}

// =============================================================================
// Main body
// =============================================================================

module body() {
    difference() {
        // Outer shell — slight corner radius for premium feel
        translate([0, 0, OH/2])
            rounded_box(OW, OD, OH, CR);

        // Inner cavity — open at top
        translate([0, 0, WALL + IH/2])
            cube([IW, ID, IH + 0.1], center=true);   // +0.1 clears top face

        // --- Front face features ---
        // USB-C slot: 9 mm wide x 4 mm tall, centered on front face, XIAO position
        // XIAO sits centered X, top of inner cavity minus 10 mm
        translate([0, -(OD/2), OH - 14])
            cube([9.5, WALL*3, 4.5], center=true);

        // Acoustic mic port: 1.8 mm dia, 3 mm from USB-C edge toward center
        translate([XIAO_W/2 - 3, -(OD/2), OH - 14 + 3])
            rotate([90, 0, 0])
                cylinder(d=MIC_PORT_D, h=WALL*3, center=true, $fn=FN_HI);

        // Round display cutout centered on front face, vertically centered lower half
        translate([0, -(OD/2), OH*0.38])
            rotate([90, 0, 0])
                cylinder(d=DISP_D_OUTER, h=WALL*3, center=true, $fn=FN_HI);

        // LED slit above display cutout
        translate([0, -(OD/2), OH - LED_Y_FROM_TOP - LED_H/2])
            cube([LED_W, WALL*3, LED_H], center=true);

        // --- Side/back speaker grille (right side wall) ---
        // Speaker module sits on right side of interior, dome facing right wall
        translate([OW/2, 0, OH*0.5])
            rotate([0, 90, 0])
                speaker_grille(6, 6, GRL_PITCH, GRL_D);

        // --- Boss drill holes (4 corners for M2 inserts, in body floor) ---
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), WALL/2])
                cylinder(d=M2_INSERT_D, h=BOSS_H + 1, $fn=FN_LO);
    }

    // --- M2 boss pillars (inside body, 4 corners) ---
    for (sx = [-1, 1]) for (sy = [-1, 1])
        translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), WALL])
            difference() {
                cylinder(d=BOSS_OD, h=BOSS_H, $fn=FN_LO);
                cylinder(d=M2_INSERT_D, h=BOSS_H + 0.1, $fn=FN_LO);
            }

    // --- Battery tray floor ribs (keep battery from sliding) ---
    for (sx = [-1, 1])
        translate([sx * (BAT_W/2 + 1), 0, WALL + BAT_H/2])
            cube([1.5, BAT_D + 2, BAT_H], center=true);
}

// =============================================================================
// Lid
// =============================================================================

module lid() {
    difference() {
        translate([0, 0, LID_T/2])
            rounded_box(OW, OD, LID_T, CR);

        // M2 clearance holes at same boss XY positions
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), -0.1])
                cylinder(d=M2_DRILL, h=LID_T + 0.2, $fn=FN_LO);
    }

    // Alignment lip (drops 2 mm into body opening)
    difference() {
        translate([0, 0, -2])
            cube([IW - 0.4, ID - 0.4, 2], center=true);
        translate([0, 0, -2.1])
            cube([IW - 0.4 - 2*WALL, ID - 0.4 - 2*WALL, 2.2], center=true);
    }
}

// =============================================================================
// Blank front plate (Phase 1) — snaps into the 39 mm cutout
// =============================================================================

module blank_front_plate() {
    difference() {
        // Plate body: 37 mm dia, 3 mm thick (1 mm proud + 2 mm lip behind wall)
        cylinder(d=DISP_D_PLATE + 4, h=3, center=false, $fn=FN_HI);

        // Snap-fit groove: thin ring cut around circumference for friction fit
        translate([0, 0, 1.5])
            difference() {
                cylinder(d=DISP_D_OUTER + 0.4, h=1.2, center=true, $fn=FN_HI);
                cylinder(d=DISP_D_OUTER - 2, h=1.4, center=true, $fn=FN_HI);
            }

        // Decorative arc-reactor circle (shallow emboss 0.4 mm deep)
        translate([0, 0, 2.7])
            cylinder(d=24, h=0.5, center=false, $fn=FN_HI);
        translate([0, 0, 2.7])
            cylinder(d=12, h=0.5, center=false, $fn=FN_HI);
    }
}

// =============================================================================
// Phase 3 screen bezel ring (thin cosmetic ring around the display)
// =============================================================================

module phase3_screen_bezel() {
    // 1.5 mm thick ring, 39 mm outer dia, 33 mm inner dia (covers display edge)
    difference() {
        cylinder(d=DISP_D_OUTER + 3, h=2.5, center=false, $fn=FN_HI);
        cylinder(d=33.0, h=3, center=false, $fn=FN_HI);  // 33 mm clear of 32 mm glass
    }
}

// =============================================================================
// Render calls — comment/uncomment to export individual parts
// =============================================================================

body();

translate([0, OD + 10, 0])
    lid();

translate([OW + 10, 0, 0])
    blank_front_plate();

translate([OW + 10, OD + 10, 0])
    phase3_screen_bezel();
