#!/bin/bash -e
set -e
#
# build_die.sh - Build NVMeVirt kernel module with die-contention timing enabled.
#
# Usage:
#   ./build_die.sh die_all       # integrated: chain + QLC hot/cold + repromotion + QLC rebalance
#   ./build_die.sh die_no1       # legacy no1: chain only
#   ./build_die.sh die_no2       # legacy no2: QLC hot/cold + repromotion + QLC rebalance, no chain
#   ./build_die.sh die_no3       # legacy no3: QLC hot/cold + repromotion, no chain/rebalance
#   ./build_die.sh die_base      # legacy baseline
#   ./build_die.sh die_base_sb   # baseline with superblock free-line accounting
#   ./build_die.sh die_latency_sb # baseline_sb + idle-aware preemptible SLC maintenance scheduler
#   ./build_die.sh die_latency_sb_v1 # same source, V2 per-die dispatcher disabled
#   ./build_die.sh die_no1_sb    # chain-only with superblock accounting and 14 active superblocks
#   ./build_die.sh die_base_lru  # baseline + 8MiB demand-loaded mapping CMT + LRU
#   ./build_die.sh die_no4       # structured GTD + SLC-metadata-log + QLC flash mappings
#   ./build_die.sh die_i_*       # integrated ablations from conv_ftl.c, using compile-time flags
#   ./build_die.sh all           # build all legacy + integrated ablation variants
#   ./build_die.sh ablations     # build only integrated ablation variants
#
# All variants use ssd_die.c which adds per-die conflict counters
# and a runtime gc_nand_timing toggle via sysfs.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
BUILD_TMPDIR=""

cleanup_staged_sources() {
    if [[ -n "${BUILD_TMPDIR:-}" && -d "$BUILD_TMPDIR" ]]; then
        for f in "$BUILD_TMPDIR"/nvmev_die_*.ko; do
            [[ -f "$f" ]] && cp "$f" .
        done
        rm -rf "$BUILD_TMPDIR"
        BUILD_TMPDIR=""
    fi
    if [[ -f ssd.c.bak ]]; then
        mv ssd.c.bak ssd.c
    fi
    if [[ -f conv_ftl.c.bak ]]; then
        mv conv_ftl.c.bak conv_ftl.c
    fi
}

trap cleanup_staged_sources EXIT

# Map variant name to conv_ftl source file
ftl_source_for() {
    case "$1" in
        die_all)  echo "conv_ftl.c"          ;;
        die_no1)  echo "conv_ftl_no1.c"      ;;
        die_no1_sb) echo "conv_ftl_no1_superblock.c" ;;
        die_no2)  echo "conv_ftl_no2.c"      ;;
        die_no3)  echo "conv_ftl_no3.c"      ;;
        die_base) echo "conv_ftl_baseline.c"  ;;
        die_base_sb) echo "conv_ftl_baseline_superblock.c" ;;
        die_latency_sb) echo "conv_ftl_latency_superblock.c" ;;
        die_latency_sb_v1) echo "conv_ftl_latency_superblock.c" ;;
        die_base_lru) echo "conv_ftl_baseline_lru.c" ;;
        die_no4) echo "conv_ftl_no4.c"       ;;
        die_base2) echo "conv_ftl_base2.c"   ;;
        die_base3) echo "conv_ftl_base3.c"   ;;
        die_i_base) echo "conv_ftl.c"        ;;
        die_i_all) echo "conv_ftl.c"         ;;
        die_i_no_chain) echo "conv_ftl.c"    ;;
        die_i_no_hotcold) echo "conv_ftl.c"  ;;
        die_i_no_repromote) echo "conv_ftl.c" ;;
        die_i_no_rebalance) echo "conv_ftl.c" ;;
        die_i_chain_only) echo "conv_ftl.c"  ;;
        die_i_no2_only) echo "conv_ftl.c"    ;;
        die_i_no3_only) echo "conv_ftl.c"    ;;
        *) echo "UNKNOWN"; return 1           ;;
    esac
}

variant_flags_for() {
    case "$1" in
        die_i_base)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=0 -DNVMEV_ENABLE_QLC_HOTCOLD=0 -DNVMEV_ENABLE_READ_REPROMOTION=0 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=0 -DNVMEV_ENABLE_QLC_REBALANCE=0"
            ;;
        die_latency_sb_v1)
            echo "-DNVMEV_LATENCY_V2_ENABLE=0"
            ;;
        die_all|die_i_all)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=1 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=1 -DNVMEV_ENABLE_QLC_REBALANCE=1"
            ;;
        die_i_no_chain)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=0 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=1 -DNVMEV_ENABLE_QLC_REBALANCE=1"
            ;;
        die_i_no_hotcold)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=1 -DNVMEV_ENABLE_QLC_HOTCOLD=0 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=1 -DNVMEV_ENABLE_QLC_REBALANCE=1"
            ;;
        die_i_no_repromote)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=1 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=0 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=0 -DNVMEV_ENABLE_QLC_REBALANCE=1"
            ;;
        die_i_no_rebalance)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=1 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=1 -DNVMEV_ENABLE_QLC_REBALANCE=0"
            ;;
        die_i_chain_only)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=1 -DNVMEV_ENABLE_QLC_HOTCOLD=0 -DNVMEV_ENABLE_READ_REPROMOTION=0 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=0 -DNVMEV_ENABLE_QLC_REBALANCE=0"
            ;;
        die_i_no2_only)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=0 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=0 -DNVMEV_ENABLE_QLC_REBALANCE=1"
            ;;
        die_i_no3_only)
            echo "-DNVMEV_ENABLE_CHAIN_AGGREGATION=0 -DNVMEV_ENABLE_QLC_HOTCOLD=1 -DNVMEV_ENABLE_READ_REPROMOTION=1 -DNVMEV_ENABLE_DIE_BATCHED_REPROMOTION=0 -DNVMEV_ENABLE_QLC_REBALANCE=0"
            ;;
        *)
            echo ""
            ;;
    esac
}

build_one() {
    local variant="$1"
    local ftl_src
    local variant_flags
    ftl_src="$(ftl_source_for "$variant")"
    variant_flags="$(variant_flags_for "$variant")"

    if [[ ! -f "$ftl_src" ]]; then
        echo "ERROR: $ftl_src not found for variant $variant" >&2
        return 1
    fi

    echo ""
    echo "=== Building nvmev_${variant}.ko  (ftl=$ftl_src flags=${variant_flags:-none}) ==="

    # Save previously built die .ko files before make clean wipes them
    local tmpdir
    tmpdir="$(mktemp -d)"
    BUILD_TMPDIR="$tmpdir"
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
    make APPROACH=on NVMEV_EXTRA_CFLAGS="$variant_flags"

    cp nvmev.ko "nvmev_${variant}.ko"
    echo "=== Built: nvmev_${variant}.ko ==="

    # Restore previously built die .ko files
    for f in "$tmpdir"/nvmev_die_*.ko; do
        [[ -f "$f" ]] && cp "$f" .
    done
    rm -rf "$tmpdir"
    BUILD_TMPDIR=""

    cp ssd.c.bak ssd.c
    cp conv_ftl.c.bak conv_ftl.c
    rm -f ssd.c.bak conv_ftl.c.bak
}

ABLATION_VARIANTS="die_i_base die_i_all die_i_no_chain die_i_no_hotcold die_i_no_repromote die_i_no_rebalance die_i_chain_only die_i_no2_only die_i_no3_only"

if [[ "${1:-}" == "all" ]]; then
    for v in die_base die_base_sb die_latency_sb die_latency_sb_v1 die_base_lru die_no4 die_base2 die_base3 die_no1 die_no1_sb die_no2 die_no3 die_all $ABLATION_VARIANTS; do
        build_one "$v"
    done
    echo ""
    echo "=== All variants built ==="
    ls -lh nvmev_die_*.ko
elif [[ "${1:-}" == "ablations" ]]; then
    for v in $ABLATION_VARIANTS; do
        build_one "$v"
    done
    echo ""
    echo "=== Ablation variants built ==="
    ls -lh nvmev_die_*.ko
elif [[ "$#" -gt 1 ]]; then
    for v in "$@"; do
        ftl_source_for "$v" >/dev/null || { echo "Unknown variant '$v'" >&2; exit 1; }
        build_one "$v"
    done
else
    VARIANT="${1:?Usage: $0 die_all|die_no1|die_no1_sb|die_no2|die_no3|die_no4|die_base|die_base_sb|die_latency_sb|die_latency_sb_v1|die_base_lru|die_base2|die_base3|die_i_base|die_i_all|die_i_no_chain|die_i_no_hotcold|die_i_no_repromote|die_i_no_rebalance|die_i_chain_only|die_i_no2_only|die_i_no3_only|all|ablations [more variants...]}"
    ftl_source_for "$VARIANT" >/dev/null || { echo "Unknown variant '$VARIANT'" >&2; exit 1; }
    build_one "$VARIANT"
fi
