#!/usr/bin/env bash
# export-stl.sh — export printable STL parts from the OpenSCAD enclosure sources
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/hardware/enclosure/dist"
TMP="$ROOT/.build_logs/stl-export"
TARGET="${1:-all}"

log() { printf '\033[1;36m[stl]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[stl]\033[0m %s\n' "$*" >&2; exit 1; }

OPENSCAD_BIN="${OPENSCAD_BIN:-$(command -v openscad || true)}"
OPENSCAD_IMAGE="${OPENSCAD_IMAGE:-openscad/openscad:latest}"

mkdir -p "$DIST" "$TMP"

run_openscad() {
    local output="$1"
    local wrapper="$2"

    if [ -n "$OPENSCAD_BIN" ]; then
        "$OPENSCAD_BIN" -o "$output" "$wrapper"
        return
    fi

    if command -v docker >/dev/null 2>&1; then
        docker run --rm \
            -v "$ROOT":/workspace \
            -w /workspace \
            "$OPENSCAD_IMAGE" \
            openscad -o "/workspace/${output#$ROOT/}" "/workspace/${wrapper#$ROOT/}"
        return
    fi

    die "OpenSCAD CLI not found. Install openscad or provide OPENSCAD_BIN=/path/to/openscad."
}

export_part() {
    local slug="$1"
    local source="$2"
    local call="$3"
    local source_abs="$ROOT/$source"
    local out="$DIST/$slug.stl"
    local lib="$TMP/$slug.lib.scad"
    local wrapper="$TMP/$slug.scad"

    [ -f "$source_abs" ] || die "missing SCAD source: $source"

    awk '/^\/\/ (Render calls|ASSEMBLY)/ { exit } { print }' "$source_abs" > "$lib"
    {
        printf 'include <%s>\n\n' "$(basename "$lib")"
        printf '%s\n' "$call"
    } > "$wrapper"

    log "exporting $slug.stl"
    run_openscad "$out" "$wrapper"
    [ -s "$out" ] || die "OpenSCAD produced an empty STL: $out"
}

export_xiao_monolith() {
    local src="hardware/enclosure/concept-1-monolith/enclosure.scad"
    export_part "xiao-monolith-body" "$src" "body();"
    export_part "xiao-monolith-lid" "$src" "lid();"
    export_part "xiao-monolith-front-plate" "$src" "blank_front_plate();"
    export_part "xiao-monolith-screen-bezel" "$src" "phase3_screen_bezel();"
}

export_xiao_cube() {
    local src="hardware/enclosure/concept-2-cube/enclosure.scad"
    export_part "xiao-open-cube-body" "$src" "body();"
    export_part "xiao-open-cube-lid" "$src" "lid();"
    export_part "xiao-open-cube-pcb-tray" "$src" "pcb_tray();"
    export_part "xiao-open-cube-front-plate" "$src" "blank_front_plate();"
    export_part "xiao-open-cube-screen-bezel" "$src" "phase3_screen_bezel();"
}

export_xiao_egg() {
    local src="hardware/enclosure/concept-3-egg/enclosure.scad"
    export_part "xiao-egg-body" "$src" "body();"
    export_part "xiao-egg-lid" "$src" "lid();"
    export_part "xiao-egg-front-plate" "$src" "blank_front_plate();"
    export_part "xiao-egg-screen-bezel" "$src" "phase3_screen_bezel();"
}

export_xiao_radio() {
    local src="hardware/enclosure/concept-4-radio/enclosure.scad"
    export_part "xiao-radio-body" "$src" "body();"
    export_part "xiao-radio-rear-panel" "$src" "lid();"
    export_part "xiao-radio-front-plate" "$src" "blank_front_plate();"
    export_part "xiao-radio-screen-bezel" "$src" "phase3_screen_bezel();"
}

export_amoled_bust() {
    local src="hardware/enclosure/amoled-1_75/concept-5-mascot-bust/enclosure.scad"
    export_part "amoled-1_75-mascot-bust-head-shell" "$src" "head_shell();"
    export_part "amoled-1_75-mascot-bust-neck" "$src" "neck();"
    export_part "amoled-1_75-mascot-bust-body" "$src" "body_shell();"
    export_part "amoled-1_75-mascot-bust-base" "$src" "base_pedestal();"
    export_part "amoled-1_75-mascot-bust-accent-ring" "$src" "accent_ring();"
}

case "$TARGET" in
    all)
        export_amoled_bust
        export_xiao_monolith
        export_xiao_cube
        export_xiao_egg
        export_xiao_radio
        ;;
    amoled|amoled-bust|mascot-bust)
        export_amoled_bust
        ;;
    xiao)
        export_xiao_monolith
        export_xiao_cube
        export_xiao_egg
        export_xiao_radio
        ;;
    monolith)
        export_xiao_monolith
        ;;
    cube|open-cube)
        export_xiao_cube
        ;;
    egg)
        export_xiao_egg
        ;;
    radio)
        export_xiao_radio
        ;;
    *)
        die "unknown target '$TARGET' (use all, amoled, xiao, monolith, cube, egg, radio)"
        ;;
esac

cat > "$DIST/README.md" <<'EOF'
# JarvisNano STL Exports

Generated from the OpenSCAD sources with:

```bash
./scripts/export-stl.sh all
```

The AMOLED-1.75 mascot bust is the primary enclosure for the Waveshare
ESP32-S3-Touch-AMOLED-1.75 board. The XIAO Monolith is the primary enclosure
for the Seeed XIAO ESP32-S3 Sense board.

Recommended first prints:

- `amoled-1_75-mascot-bust-head-shell.stl`
- `amoled-1_75-mascot-bust-body.stl`
- `amoled-1_75-mascot-bust-base.stl`
- `amoled-1_75-mascot-bust-accent-ring.stl`
- `xiao-monolith-body.stl`
- `xiao-monolith-lid.stl`

Open each STL in the slicer and orient the largest flat face on the bed before
printing. Use PETG/ASA for production shells and PLA for quick fit checks.
EOF

log "done → $DIST"
