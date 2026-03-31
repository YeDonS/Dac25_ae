#!/bin/bash -e
#
# build_die.sh - Build NVMeVirt kernel module with die-contention timing enabled.
#
# Usage:
#   ./build_die.sh die_all       # repromotion ON  + QLC internal migration ON
#   ./build_die.sh die_no1       # repromotion OFF + QLC internal migration OFF
#   ./build_die.sh die_no2       # repromotion ON  + QLC internal migration ON  (variant 2)
#   ./build_die.sh die_no3       # repromotion ON  + QLC internal migration OFF
#   ./build_die.sh die_base      # repromotion OFF + QLC internal migration OFF (baseline)
#   ./build_die.sh die_base2     # baseline + random die placement for internal moves
#   ./build_die.sh die_base3     # baseline + shared host lunpointer for internal moves
#   ./build_die.sh all           # build all seven variants
#
# All variants use ssd_die.c which adds per-die conflict counters
# and a runtime gc_nand_timing toggle via sysfs.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Map variant name to conv_ftl source file
ftl_source_for() {
    case "$1" in
        die_all)  echo "conv_ftl.c"          ;;
        die_no1)  echo "conv_ftl_no1.c"      ;;
        die_no2)  echo "conv_ftl_no2.c"      ;;
        die_no3)  echo "conv_ftl_no3.c"      ;;
        die_base) echo "conv_ftl_baseline.c"  ;;
        die_base2) echo "conv_ftl_base2.c"   ;;
        die_base3) echo "conv_ftl_base3.c"   ;;
        *) echo "UNKNOWN"; return 1           ;;
    esac
}

build_one() {
    local variant="$1"
    local ftl_src
    ftl_src="$(ftl_source_for "$variant")"

    if [[ ! -f "$ftl_src" ]]; then
        echo "ERROR: $ftl_src not found for variant $variant" >&2
        return 1
    fi

    echo ""
    echo "=== Building nvmev_${variant}.ko  (ftl=$ftl_src) ==="

    # Save previously built die .ko files before make clean wipes them
    local tmpdir
    tmpdir="$(mktemp -d)"
    for f in nvmev_die_*.ko; do
        [[ -f "$f" ]] && cp "$f" "$tmpdir/"
    done

    cp ssd.c ssd.c.bak
    cp conv_ftl.c conv_ftl.c.bak

    cp ssd_die.c ssd.c
    if [[ "$ftl_src" != "conv_ftl.c" ]]; then
        cp "$ftl_src" conv_ftl.c
    else
        cp conv_ftl.c.bak conv_ftl.c
    fi

    make clean
    make APPROACH=on

    cp nvmev.ko "nvmev_${variant}.ko"
    echo "=== Built: nvmev_${variant}.ko ==="

    # Restore previously built die .ko files
    for f in "$tmpdir"/nvmev_die_*.ko; do
        [[ -f "$f" ]] && cp "$f" .
    done
    rm -rf "$tmpdir"

    cp ssd.c.bak ssd.c
    cp conv_ftl.c.bak conv_ftl.c
    rm -f ssd.c.bak conv_ftl.c.bak
}

if [[ "${1:-}" == "all" ]]; then
    for v in die_base die_base2 die_base3 die_no1 die_no2 die_no3 die_all; do
        build_one "$v"
    done
    echo ""
    echo "=== All variants built ==="
    ls -lh nvmev_die_*.ko
else
    VARIANT="${1:?Usage: $0 die_all|die_no1|die_no2|die_no3|die_base|die_base2|die_base3|all}"
    ftl_source_for "$VARIANT" >/dev/null || { echo "Unknown variant '$VARIANT'" >&2; exit 1; }
    build_one "$VARIANT"
fi
