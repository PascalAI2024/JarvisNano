// =============================================================================
// J.A.R.V.I.S. Enclosure — Concept 2: Open Cube
// Aluminum-look PLA body, clear acrylic top plate, hex M2 cap-heads visible.
// PCB tray: removable XIAO carrier slides out from the top.
// Speaker fires forward through a hex-pattern grille.
// Sharp 90-degree corners — industrial, precise.
// =============================================================================

// --- Global parameters -------------------------------------------------------
WALL  = 2.0;
FN_HI = 64;
FN_LO = 32;

// --- Outer envelope ----------------------------------------------------------
OW = 76.0;   // Width  (X)
OD = 66.0;   // Depth  (Y)
OH = 65.0;   // Height (Z) — slightly squarer than Monolith

// --- Inner cavity ------------------------------------------------------------
IW = OW - 2*WALL;   // 72.0
ID = OD - 2*WALL;   // 62.0
IH = OH - WALL;     // 63.0 (open top)

// --- Component dimensions (canonical) ----------------------------------------
XIAO_W = 21.0;
XIAO_D = 17.5;
XIAO_H = 3.5;

MIC_PORT_D = 1.8;

SPK_W  = 50.0;
SPK_D  = 30.0;
SPK_H  = 18.0;
SPK_DOME_D = 28.0;

GRL_D     = 2.0;
GRL_PITCH = 3.8;

BAT_W = 52.0;
BAT_D = 35.0;
BAT_H = 6.0;

DISP_D_OUTER = 39.0;
DISP_D_PLATE = 37.0;

// LED indicator bar (front, flush strip)
LED_W = 30.0;
LED_H = 2.0;

// M2 hardware
BOSS_OD    = 6.0;
BOSS_H     = 8.0;
M2_DRILL   = 2.2;
M2_INSERT_D = 3.2;

LID_T = 3.0;  // acrylic-style thick top

// Tray rail dimensions
RAIL_W = 2.0;
RAIL_H = 3.0;

// =============================================================================
// Utility: hex grid (for industrial grille)
// =============================================================================

module hex_grille(rows, cols, pitch, hole_d, depth) {
    for (row = [0 : rows-1]) {
        for (col = [0 : cols-1]) {
            x = (col - (cols-1)/2) * pitch + (row % 2) * pitch/2;
            y = (row - (rows-1)/2) * pitch * 0.866;
            translate([x, y, 0])
                cylinder(d=hole_d, h=depth + 0.2, center=true, $fn=6);
        }
    }
}

// =============================================================================
// PCB tray (slides into body from top, carries XIAO on standoffs)
// =============================================================================

module pcb_tray() {
    tray_w = IW - 2;
    tray_d = 26.0;   // just fits XIAO depth + cable bend
    tray_h = XIAO_H + 4;  // 4 mm clearance under board

    difference() {
        cube([tray_w, tray_d, tray_h], center=true);
        // Hollow interior
        translate([0, 0, 1])
            cube([tray_w - 2*WALL, tray_d - 2*WALL, tray_h], center=true);
        // XIAO footprint cutout in floor (component underside clearance)
        translate([0, 0, -(tray_h/2)])
            cube([XIAO_W + 1, XIAO_D + 1, 3], center=true);
    }

    // Standoffs for XIAO (4 corners, 2 mm tall)
    for (sx = [-1, 1]) for (sy = [-1, 1])
        translate([sx*(XIAO_W/2 - 1.5), sy*(XIAO_D/2 - 1.5), -(tray_h/2) + 2])
            cylinder(d=2, h=2, $fn=FN_LO);
}

// =============================================================================
// Body
// =============================================================================

module body() {
    difference() {
        // Outer shell — sharp corners (Cube identity)
        cube([OW, OD, OH], center=true);

        // Inner cavity
        translate([0, 0, WALL/2 + 0.05])
            cube([IW, ID, IH + 0.1], center=true);

        // --- Front wall features ---
        // USB-C slot
        translate([0, -(OD/2), OH/2 - 12])
            cube([9.5, WALL*3, 4.5], center=true);

        // Mic acoustic port
        translate([XIAO_W/2 - 3, -(OD/2), OH/2 - 12 + 3])
            rotate([90, 0, 0])
                cylinder(d=MIC_PORT_D, h=WALL*3, center=true, $fn=FN_HI);

        // Round display cutout — front face, center-low
        translate([0, -(OD/2), -OH/2 + OH*0.38])
            rotate([90, 0, 0])
                cylinder(d=DISP_D_OUTER, h=WALL*3, center=true, $fn=FN_HI);

        // LED strip slot
        translate([0, -(OD/2), -OH/2 + OH*0.38 + DISP_D_OUTER/2 + 7])
            cube([LED_W, WALL*3, LED_H], center=true);

        // --- Front hex speaker grille (speaker fires forward, left side)
        translate([-(OW/2 - SPK_W/2 - WALL), -(OD/2), -OH/2 + SPK_H/2 + WALL])
            rotate([90, 0, 0])
                hex_grille(5, 8, GRL_PITCH, GRL_D, WALL*2);

        // --- Top rim: 4 M2 clearance holes (through top rim for lid screws) ---
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), OH/2])
                cylinder(d=M2_DRILL, h=WALL*3, center=true, $fn=FN_LO);

        // --- Back wall: cable management slot ---
        translate([0, OD/2, -OH/2 + 12])
            cube([20, WALL*3, 8], center=true);
    }

    // --- M2 boss pillars (inside, 4 corners, from floor) ---
    for (sx = [-1, 1]) for (sy = [-1, 1])
        translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), -OH/2 + WALL])
            difference() {
                cylinder(d=BOSS_OD, h=BOSS_H, $fn=FN_LO);
                cylinder(d=M2_INSERT_D, h=BOSS_H + 0.1, $fn=FN_LO);
            }

    // --- Tray rails (left and right inner walls, for PCB tray to slide down) ---
    for (sx = [-1, 1])
        translate([sx*(IW/2 - RAIL_W/2), 0, -OH/2 + WALL + (IH - RAIL_H)/2])
            cube([RAIL_W, ID, RAIL_H], center=true);

    // Battery retention ribs
    for (sx = [-1, 1])
        translate([sx*(BAT_W/2 + 1), 0, -OH/2 + WALL + BAT_H/2])
            cube([1.5, BAT_D + 2, BAT_H], center=true);
}

// =============================================================================
// Lid (mimics clear acrylic panel — thin, flat, exposed screws)
// =============================================================================

module lid() {
    difference() {
        cube([OW, OD, LID_T], center=true);

        // M2 countersink holes (hex cap heads are the aesthetic)
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx*(IW/2 - BOSS_OD/2 - 1), sy*(ID/2 - BOSS_OD/2 - 1), 0])
                cylinder(d=M2_DRILL, h=LID_T + 0.2, center=true, $fn=FN_LO);

        // Optional: rectangular window for PCB visibility (open cube concept)
        translate([0, 0, 0])
            cube([IW - 8, ID - 8, LID_T + 0.2], center=true);
    }
}

// =============================================================================
// Blank front plate (Phase 1)
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
        // Decorative "J" — represented as text-like emboss circle set
        translate([0, 0, 3.2])
            cylinder(d=18, h=0.4, center=false, $fn=FN_HI);
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

translate([0, 0, OH/2]) body();

translate([0, OD + 15, LID_T/2]) lid();

translate([OW + 15, 0, 0]) blank_front_plate();

translate([OW + 15, OD + 15, 0]) phase3_screen_bezel();

translate([0, -(OD/2 + 30), 0]) pcb_tray();
