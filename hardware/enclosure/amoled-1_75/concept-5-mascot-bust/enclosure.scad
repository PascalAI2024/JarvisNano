// J.A.R.V.I.S. Mascot Bust — AMOLED-1.75 Enclosure (Concept 5)
//
// Chibi character bust where the AMOLED is the face.
// All dimensions in millimeters, derived from Waveshare's authoritative
// ESP32-S3-Touch-AMOLED-1.75 3D drawing.
//
// Render with:
//   openscad -o enclosure.stl enclosure.scad
//
// To export individual parts, comment / uncomment the calls at the bottom.

// =============================================================================
// PARAMETRIC INPUTS — tune these without re-modeling
// =============================================================================

// --- Board (from Waveshare's drawing) ---
pcb_diameter           = 46.0;
pcb_thickness          = 1.10;
cover_glass_diameter   = 48.96;       // AMOLED bezel sits proud of PCB
cover_glass_thickness  = 2.05;        // CG + display
board_stack_height     = 10.0;        // total Z, both sides

// --- Mounting (3 x M2 on a bolt circle, 120 deg apart) ---
mount_hole_radius      = 23.0 - 2.5;  // 23.0 is OD; pull in for inner ring location
mount_hole_count       = 3;
mount_hole_dia         = 2.2;         // through hole for M2 self-tap; for heat-set use 3.3
mount_hole_phase_deg   = 30;          // rotate the triad so screws aren't on cardinal axes

// --- Head shell ---
head_outer_diameter    = 60.0;
head_wall              = 2.4;
head_depth             = 38.0;        // front-to-back inside the head
head_face_recess       = 0.5;         // how far the AMOLED cover-glass sinks below front edge

// --- Neck (head-to-body) ---
neck_diameter          = 22.0;
neck_height            = 12.0;

// --- Body ---
body_w                 = 50.0;
body_d                 = 30.0;
body_h                 = 50.0;
body_wall              = 2.4;
body_corner_radius     = 5.0;

// --- Speaker (28 mm dia, 18 mm deep) ---
speaker_diameter       = 28.0;
speaker_chamber_depth  = 18.0;
chest_grille_count     = 5;           // diagonal slot rows on the chest
chest_grille_slot_w    = 2.0;
chest_grille_slot_h    = 12.0;
chest_grille_angle     = 25;          // diagonal degrees

// --- Battery cavity (503450 LiPo + clearance) ---
battery_w              = 52.0;
battery_d              = 35.0;
battery_h              = 6.5;

// --- Base pedestal ---
base_w                 = 70.0;
base_d                 = 50.0;
base_h                 = 15.0;
base_corner_radius     = 8.0;
usb_grommet_dia        = 12.0;        // USB-C cable exit, back of base

// --- Microphone ports (2 x MEMS) ---
mic_port_dia           = 1.8;
mic_port_angle_up      = 12;          // tilt upward to catch voice from above
mic_port_side_offset   = 24.0;        // half-angle from front centerline

// --- Side buttons (PWR / BOOT) ---
side_button_dia        = 4.0;         // outer cap for finger
side_button_inset      = 0.6;
pwr_button_angle       = 90;          // right side of head
boot_button_angle      = -90;         // left side

// --- Misc ---
$fn                    = 96;          // segment count for circles
eps                    = 0.01;        // overlap fudge

// =============================================================================
// HEAD SHELL
// =============================================================================
//
// A flattened sphere — taller than wide is mascot-cute. AMOLED set into the
// front face. Mounting boss ring on the back side of the front wall.
//
module head_shell() {
    difference() {
        union() {
            // Outer head — a capsule (squashed sphere)
            scale([1, 0.95, 1.08])
                sphere(d = head_outer_diameter);
        }

        // Hollow interior — inner volume so it's a SHELL
        scale([1, 0.95, 1.08])
            sphere(d = head_outer_diameter - 2 * head_wall);

        // Cover-glass cutout — clear window for the AMOLED
        translate([0, head_outer_diameter / 2 - head_face_recess + eps, 0])
            rotate([-90, 0, 0])
                cylinder(h = head_wall + 2,
                         d = cover_glass_diameter + 0.6, center = false);

        // Neck stub cutout (lower opening)
        translate([0, 0, -head_outer_diameter / 2 - eps])
            cylinder(h = neck_height + 4, d = neck_diameter + 0.4);

        // Mic ports — 2 angled holes at the upper-side cheeks
        for (side = [-1, 1]) {
            rotate([0, 0, side * mic_port_side_offset])
                rotate([-mic_port_angle_up, 0, 0])
                    translate([0, head_outer_diameter / 2 - head_wall - 2, head_outer_diameter * 0.18])
                        rotate([90, 0, 0])
                            cylinder(h = head_wall + 4, d = mic_port_dia, center = false);
        }

        // Side button ports (PWR + BOOT) — small flush caps
        for (ang = [pwr_button_angle, boot_button_angle]) {
            rotate([0, 0, ang])
                translate([0, head_outer_diameter / 2 - head_wall + eps, 0])
                    rotate([90, 0, 0])
                        cylinder(h = head_wall + 4, d = side_button_dia, center = false);
        }
    }

    // Inner mounting ring (3 bosses) — sits behind the AMOLED cutout, board screws into it
    head_inner_mount_ring();
}

module head_inner_mount_ring() {
    // The boss plane is offset back from the front face by cover_glass_thickness
    boss_plane_y = head_outer_diameter / 2 - head_face_recess - cover_glass_thickness;
    boss_h       = 4.0;

    for (i = [0 : mount_hole_count - 1]) {
        ang = mount_hole_phase_deg + i * (360 / mount_hole_count);
        rotate([0, 0, ang])
            translate([mount_hole_radius, boss_plane_y, 0])
                rotate([90, 0, 0]) {
                    difference() {
                        cylinder(h = boss_h, d = mount_hole_dia + 4);
                        translate([0, 0, -eps])
                            cylinder(h = boss_h + 2, d = mount_hole_dia);
                    }
                }
    }
}

// =============================================================================
// NECK
// =============================================================================
module neck() {
    translate([0, 0, -head_outer_diameter / 2 + 2])
        cylinder(h = neck_height + 4, d = neck_diameter);
}

// =============================================================================
// BODY
// =============================================================================
module body_shell() {
    body_z_top = -head_outer_diameter / 2 + 2 - neck_height;

    translate([0, 0, body_z_top - body_h])
        difference() {
            // Outer body, rounded rectangle
            rounded_box(body_w, body_d, body_h, body_corner_radius);

            // Hollow inside
            translate([0, 0, body_wall])
                rounded_box(body_w - 2 * body_wall,
                            body_d - 2 * body_wall,
                            body_h - body_wall, body_corner_radius - 1);

            // Speaker chamber recess — fires forward through chest grille
            translate([0, -body_d / 2 + body_wall - eps, body_h / 2])
                rotate([-90, 0, 0])
                    cylinder(h = body_d, d = speaker_diameter);

            // Battery cavity (back side, accessible from inside)
            translate([0, body_d / 2 - battery_d / 2 - body_wall, battery_h / 2 + body_wall])
                cube([battery_w, battery_d, battery_h], center = true);

            // Chest grille slots — diagonal
            chest_grille();
        }
}

module chest_grille() {
    // n diagonal slots across the chest face
    slot_pitch = chest_grille_slot_h * 0.5;
    for (i = [0 : chest_grille_count - 1]) {
        x_off = (i - (chest_grille_count - 1) / 2) * slot_pitch * 1.4;
        translate([x_off, -body_d / 2 - eps, body_h / 2 + 1])
            rotate([0, chest_grille_angle, 0])
                rotate([90, 0, 0])
                    minkowski() {
                        cube([chest_grille_slot_w, chest_grille_slot_h, 0.1],
                             center = true);
                        sphere(d = chest_grille_slot_w);
                    }
    }
}

// =============================================================================
// BASE
// =============================================================================
module base_pedestal() {
    body_z_top = -head_outer_diameter / 2 + 2 - neck_height;
    base_z_top = body_z_top - body_h;

    translate([0, 0, base_z_top - base_h])
        difference() {
            rounded_box(base_w, base_d, base_h, base_corner_radius);

            // Cavity for cable management
            translate([0, 0, base_h / 2])
                rounded_box(base_w - 8, base_d - 8, base_h - 4, base_corner_radius - 2);

            // USB-C grommet, back of base
            translate([0, base_d / 2 - 2, base_h / 2])
                rotate([90, 0, 0])
                    cylinder(h = 10, d = usb_grommet_dia, center = true);
        }
}

// =============================================================================
// UTILITY: rounded box (minkowski over a smaller cube + sphere)
// =============================================================================
module rounded_box(w, d, h, r) {
    translate([0, 0, h / 2])
        minkowski() {
            cube([w - 2 * r, d - 2 * r, h - 2 * r], center = true);
            sphere(r = r);
        }
}

// =============================================================================
// ACCENT RING (printed in orange, separate part)
// =============================================================================
//
// Fits between the head shell and the cover glass — provides the orange
// "visor" trim that matches the JarvisNano mascot identity. Printed separately
// in matte orange PLA, glued or friction-fit into the cover-glass recess.
//
module accent_ring() {
    translate([0, head_outer_diameter / 2 - head_face_recess - 0.5, 0])
        rotate([-90, 0, 0])
            difference() {
                cylinder(h = 1.2, d = cover_glass_diameter + 4);
                translate([0, 0, -eps])
                    cylinder(h = 1.4, d = cover_glass_diameter + 0.4);
            }
}

// =============================================================================
// ASSEMBLY — comment out parts as needed for individual export
// =============================================================================
head_shell();
neck();
body_shell();
base_pedestal();
// accent_ring();    // Export this on its own to print in orange filament
