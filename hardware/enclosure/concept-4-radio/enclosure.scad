// =============================================================================
// J.A.R.V.I.S. Enclosure — Concept 4: Radio
// Vintage desk radio silhouette: wider than tall, fabric-like speaker grille
// on face, a cylindrical status knob on top-right, brass-face aesthetic.
// Lid = rear panel, 4x M2 screws. Speaker fires forward.
// =============================================================================

// --- Global parameters -------------------------------------------------------
WALL  = 2.0;
FN_HI = 64;
FN_LO = 32;

// --- Outer envelope ----------------------------------------------------------
// Radio is wider than tall — landscape orientation
OW = 78.0;   // Width  (X)
OD = 60.0;   // Depth  (Y)
OH = 55.0;   // Height (Z) — shorter, wider

// --- Inner cavity ------------------------------------------------------------
IW = OW - 2*WALL;
ID = OD - 2*WALL;
IH = OH - 2*WALL;

// --- Component dimensions (canonical) ----------------------------------------
XIAO_W = 21.0;
XIAO_D = 17.5;
XIAO_H = 3.5;

MIC_PORT_D = 1.8;

SPK_W  = 50.0;
SPK_D  = 30.0;
SPK_H  = 18.0;
SPK_DOME_D = 28.0;

// Radio grille: vertical slot pattern (classic radio aesthetic)
GRL_SLOT_W   = 1.5;
GRL_SLOT_H   = 22.0;
GRL_SLOT_PITCH = 3.5;
GRL_SLOT_COUNT = 10;

BAT_W = 52.0;
BAT_D = 35.0;
BAT_H = 6.0;

DISP_D_OUTER = 39.0;
DISP_D_PLATE = 37.0;

// Status knob (cylindrical protrusion on top-right)
KNOB_D = 14.0;
KNOB_H = 10.0;
KNOB_X = OW/2 - 16;
KNOB_Y = -OD/2 + 18;   // toward front

// M2 hardware
BOSS_OD     = 6.0;
BOSS_H      = 8.0;
M2_DRILL    = 2.2;
M2_INSERT_D = 3.2;

LID_T = 2.5;   // rear panel thickness

// Corner radius (Radio gets moderate fillets — classic, not harsh)
CR = 5.0;

// =============================================================================
// Utility
// =============================================================================

module rounded_box(w, d, h, r) {
    minkowski() {
        cube([w - 2*r, d - 2*r, h - 2*r], center=true);
        sphere(r=r, $fn=FN_LO);
    }
}

// Radio grille: vertical slots
module radio_grille(count, slot_w, slot_h, pitch) {
    for (i = [0 : count-1]) {
        x = (i - (count-1)/2) * pitch;
        cube([slot_w, WALL*3, slot_h], center=true);
        translate([x, 0, 0])
            cube([slot_w, WALL*3, slot_h], center=true);
    }
}

// =============================================================================
// Body (front-opening — lid is the rear panel)
// =============================================================================

module body() {
    difference() {
        // Outer shell with classic radio rounded corners
        translate([0, 0, OH/2])
            rounded_box(OW, OD, OH, CR);

        // Inner cavity (open at back)
        translate([0, WALL/2, OH/2])
            cube([IW, ID + 0.1, IH + 2*WALL], center=true);

        // --- Front face features ---
        // Round display cutout — left-center of front face (radio dial aesthetic)
        translate([-(OW/2 - DISP_D_OUTER/2 - WALL - 4), -(OD/2), OH/2])
            rotate([90, 0, 0])
                cylinder(d=DISP_D_OUTER, h=WALL*3, center=true, $fn=FN_HI);

        // Speaker grille slots — right portion of front face
        translate([OW/4, -(OD/2), OH/2])
            radio_grille(GRL_SLOT_COUNT, GRL_SLOT_W, GRL_SLOT_H, GRL_SLOT_PITCH);

        // USB-C slot — bottom edge of front face (XIAO USB-C exits through bottom-front)
        translate([-(OW/2 - XIAO_W/2 - WALL - 4), -(OD/2), WALL + 3])
            cube([9.5, WALL*3, 4.5], center=true);

        // Mic acoustic port
        translate([-(OW/2 - XIAO_W/2 - WALL - 4) + XIAO_W/2 - 3, -(OD/2), WALL + 3 + 3])
            rotate([90, 0, 0])
                cylinder(d=MIC_PORT_D, h=WALL*3, center=true, $fn=FN_HI);

        // --- Top face: status knob hole ---
        translate([KNOB_X, KNOB_Y, OH])
            cylinder(d=KNOB_D - 4, h=WALL*3, center=true, $fn=FN_HI);

        // --- Rear face: M2 clearance for lid ---
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), OD/2, sy*(IH/2 - BOSS_OD/2 - 1) + OH/2])
                rotate([90, 0, 0])
                    cylinder(d=M2_DRILL, h=WALL*3, center=true, $fn=FN_LO);
    }

    // --- M2 boss pillars (rear wall interior, 4 corners) ---
    for (sx = [-1, 1]) for (sy = [-1, 1])
        translate([sx*(IW/2 - BOSS_OD/2 - 1), OD/2 - WALL - BOSS_H, sy*(IH/2 - BOSS_OD/2 - 1) + OH/2])
            rotate([90, 0, 0])
                difference() {
                    cylinder(d=BOSS_OD, h=BOSS_H, $fn=FN_LO);
                    cylinder(d=M2_INSERT_D, h=BOSS_H + 0.1, $fn=FN_LO);
                }

    // --- Status knob (solid cylinder on top, hollow LED pipe inside) ---
    translate([KNOB_X, KNOB_Y, OH])
        difference() {
            cylinder(d=KNOB_D, h=KNOB_H, center=false, $fn=FN_HI);
            // Hollow core for LED light pipe (4 mm dia)
            cylinder(d=4, h=KNOB_H + 0.1, center=false, $fn=FN_LO);
        }

    // Battery ribs on floor
    for (sx = [-1, 1])
        translate([sx*(BAT_W/2 + 1), 0, WALL + BAT_H/2])
            cube([1.5, BAT_D + 2, BAT_H], center=true);
}

// =============================================================================
// Rear panel lid
// =============================================================================

module lid() {
    difference() {
        translate([0, 0, OH/2])
            rounded_box(OW, LID_T, OH, CR/2);

        // M2 clearance holes
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), 0, sy*(IH/2 - BOSS_OD/2 - 1) + OH/2])
                rotate([90, 0, 0])
                    cylinder(d=M2_DRILL, h=LID_T + 0.2, center=true, $fn=FN_LO);
    }

    // Alignment lip (2 mm flange that enters body opening)
    difference() {
        translate([0, LID_T/2, OH/2])
            cube([IW - 0.4, 2, IH - 0.4], center=true);
        translate([0, LID_T/2 + 0.1, OH/2])
            cube([IW - 0.4 - 2*WALL, 2.2, IH - 0.4 - 2*WALL], center=true);
    }
}

// =============================================================================
// Blank front plate (Phase 1 — snaps into the round cutout)
// =============================================================================

module blank_front_plate() {
    difference() {
        cylinder(d=DISP_D_OUTER + 2, h=3.5, center=false, $fn=FN_HI);
        // Snap groove
        translate([0, 0, 1.8])
            difference() {
                cylinder(d=DISP_D_OUTER + 0.4, h=1.2, center=true, $fn=FN_HI);
                cylinder(d=DISP_D_OUTER - 2.5, h=1.4, center=true, $fn=FN_HI);
            }
        // Decorative concentric rings (art deco radio dial)
        translate([0, 0, 3.2]) cylinder(d=30, h=0.35, center=false, $fn=FN_HI);
        translate([0, 0, 3.2]) cylinder(d=20, h=0.35, center=false, $fn=FN_HI);
        translate([0, 0, 3.2]) cylinder(d=10, h=0.35, center=false, $fn=FN_HI);
    }
}

// =============================================================================
// Phase 3 screen bezel
// =============================================================================

module phase3_screen_bezel() {
    difference() {
        cylinder(d=DISP_D_OUTER + 3, h=2.5, center=false, $fn=FN_HI);
        cylinder(d=33.0, h=3.0, center=false, $fn=FN_HI);
    }
}

// =============================================================================
// Render calls
// =============================================================================

body();

translate([OW + 15, 0, 0])
    rotate([0, 0, 90])
        lid();

translate([0, OD + 15, 0]) blank_front_plate();

translate([OW + 15, OD + 15, 0]) phase3_screen_bezel();
