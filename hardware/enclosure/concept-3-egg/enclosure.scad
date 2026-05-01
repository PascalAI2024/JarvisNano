// =============================================================================
// J.A.R.V.I.S. Enclosure — Concept 3: Egg
// Smooth organic form, heavy fillets on all edges, cleanest visual profile.
// Layout: horizontal layer stack. Lid splits at mid-height (equator).
// 4x M2 screws at equator join top and bottom halves.
// Speaker fires upward through top dome perforations.
// =============================================================================

// --- Global parameters -------------------------------------------------------
WALL  = 2.0;
FN_HI = 128;   // Egg needs smooth curves
FN_LO = 64;

// --- Outer envelope ----------------------------------------------------------
// Egg is slightly taller, slightly narrower for that pebble silhouette
OW = 72.0;
OD = 62.0;
OH = 68.0;

// Equator height (where body splits into bottom + top)
EQ_Z = 28.0;   // lower half taller to hold battery + speaker

// --- Inner cavity ------------------------------------------------------------
IW = OW - 2*WALL;   // 68.0
ID = OD - 2*WALL;   // 58.0

// --- Component dimensions (canonical) ----------------------------------------
XIAO_W = 21.0;
XIAO_D = 17.5;
XIAO_H = 3.5;

MIC_PORT_D = 1.8;

SPK_W  = 50.0;
SPK_D  = 30.0;
SPK_H  = 18.0;
SPK_DOME_D = 28.0;

GRL_D     = 1.8;
GRL_PITCH = 3.2;

BAT_W = 52.0;
BAT_D = 35.0;
BAT_H = 6.0;

DISP_D_OUTER = 39.0;

// M2 hardware
BOSS_OD     = 6.0;
BOSS_H      = 6.0;
M2_DRILL    = 2.2;
M2_INSERT_D = 3.2;

// Fillet radius for egg form
FILLET = 18.0;

// =============================================================================
// Egg body profile — built with hull() of two ellipsoidal spheres
// This creates a capsule/egg shape that is FDM-printable (rounded but no
// extreme undercuts if oriented correctly).
// =============================================================================

module egg_solid() {
    hull() {
        // Bottom lobe — wider and flatter
        translate([0, 0, OW/3])
            scale([OW/2 / (OW/3), OD/2 / (OW/3), 1])
                sphere(r=OW/3, $fn=FN_HI);
        // Top lobe — narrower
        translate([0, 0, OH - OW/4])
            scale([OW/2.4 / (OW/4), OD/2.4 / (OW/4), 1])
                sphere(r=OW/4, $fn=FN_HI);
    }
}

module egg_inner() {
    hull() {
        translate([0, 0, OW/3])
            scale([(OW/2 - WALL) / (OW/3), (OD/2 - WALL) / (OW/3), 1])
                sphere(r=OW/3, $fn=FN_HI);
        translate([0, 0, OH - OW/4])
            scale([(OW/2.4 - WALL) / (OW/4), (OD/2.4 - WALL) / (OW/4), 1])
                sphere(r=OW/4, $fn=FN_HI);
    }
}

// =============================================================================
// Bottom half (body)
// =============================================================================

module body() {
    difference() {
        // Bottom half of egg solid
        intersection() {
            egg_solid();
            translate([0, 0, EQ_Z/2])
                cube([OW + 4, OD + 4, EQ_Z], center=true);
        }

        // Hollow out
        intersection() {
            egg_inner();
            translate([0, 0, EQ_Z/2 + WALL])
                cube([OW, OD, EQ_Z], center=true);
        }

        // Front face: USB-C slot (front = -Y direction)
        // XIAO sits near top of bottom half
        translate([0, -(OD/2), EQ_Z - 6])
            cube([9.5, WALL*3, 4.5], center=true);

        // Mic acoustic port
        translate([XIAO_W/2 - 3, -(OD/2), EQ_Z - 6 + 3])
            rotate([90, 0, 0])
                cylinder(d=MIC_PORT_D, h=WALL*3, center=true, $fn=FN_HI);

        // Round display cutout: front face, vertically centered in lower portion
        translate([0, -(OD/2), EQ_Z * 0.45])
            rotate([90, 0, 0])
                cylinder(d=DISP_D_OUTER, h=WALL*3, center=true, $fn=FN_HI);

        // M2 boss drill holes (equator plane, 4 positions)
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx * 22, sy * 18, EQ_Z - 1])
                cylinder(d=M2_INSERT_D, h=BOSS_H + 1, $fn=FN_LO);
    }

    // M2 boss pillars at equator (heat-set inserts face up)
    for (sx = [-1, 1]) for (sy = [-1, 1])
        translate([sx * 22, sy * 18, WALL])
            difference() {
                cylinder(d=BOSS_OD, h=BOSS_H, $fn=FN_LO);
                cylinder(d=M2_INSERT_D, h=BOSS_H + 0.1, $fn=FN_LO);
            }

    // Battery retention ledge ribs
    for (sx = [-1, 1])
        translate([sx * (BAT_W/2 + 1), 0, WALL + BAT_H/2])
            cube([1.5, BAT_D + 2, BAT_H], center=true);
}

// =============================================================================
// Top half (lid)
// =============================================================================

module lid() {
    difference() {
        // Top half of egg solid
        intersection() {
            egg_solid();
            translate([0, 0, EQ_Z + (OH - EQ_Z)/2])
                cube([OW + 4, OD + 4, OH - EQ_Z], center=true);
        }

        // Hollow out top half (thinner — just structural shell)
        intersection() {
            egg_inner();
            translate([0, 0, EQ_Z + (OH - EQ_Z)/2 + WALL])
                cube([OW, OD, OH - EQ_Z], center=true);
        }

        // Speaker grille through top dome (speaker fires upward, offset back)
        translate([0, SPK_D/4, OH - 8])
            for (row = [0 : 4])
                for (col = [0 : 7])
                    translate([(col - 3.5)*GRL_PITCH, (row - 2)*GRL_PITCH, 0])
                        cylinder(d=GRL_D, h=WALL*3, center=true, $fn=FN_LO);

        // M2 clearance holes at equator
        for (sx = [-1, 1]) for (sy = [-1, 1])
            translate([sx * 22, sy * 18, EQ_Z])
                cylinder(d=M2_DRILL, h=BOSS_H + 2, $fn=FN_LO);
    }

    // Alignment spigot ring (drops 2 mm into body opening at equator)
    translate([0, 0, EQ_Z])
        difference() {
            // The spigot profile at the equator cross-section of the egg
            intersection() {
                egg_solid();
                translate([0, 0, EQ_Z])
                    cube([OW, OD, 4], center=true);
            }
            intersection() {
                egg_inner();
                translate([0, 0, EQ_Z + 0.5])
                    cube([OW, OD, 4], center=true);
            }
        }
}

// =============================================================================
// Blank front plate (Phase 1)
// =============================================================================

module blank_front_plate() {
    difference() {
        cylinder(d=DISP_D_OUTER + 2, h=3.5, center=false, $fn=FN_HI);
        translate([0, 0, 1.8])
            difference() {
                cylinder(d=DISP_D_OUTER + 0.4, h=1.2, center=true, $fn=FN_HI);
                cylinder(d=DISP_D_OUTER - 2.5, h=1.4, center=true, $fn=FN_HI);
            }
        // Arc reactor emboss
        translate([0, 0, 3.2])
            cylinder(d=22, h=0.4, center=false, $fn=FN_HI);
        translate([0, 0, 3.2])
            cylinder(d=10, h=0.4, center=false, $fn=FN_HI);
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

translate([0, OD + 20, OH])
    rotate([180, 0, 0])
        lid();

translate([OW + 20, 0, 0]) blank_front_plate();

translate([OW + 20, OD + 20, 0]) phase3_screen_bezel();
