#!/bin/bash -e
#
# build_die.sh - Build NVMeVirt kernel module with die-contention timing enabled.
#
# Usage:
#   ./build_die.sh die_all    # die affinity ON  + GC NAND timing ON
#   ./build_die.sh die_no2    # die affinity OFF + GC NAND timing ON
#
# Output:
#   nvmev_die_all.ko  or  nvmev_die_no2.ko
#
# Key difference from the normal build:
#   ssd_die.c removes the GC_IO early-return in ssd_advance_nand(),
#   so GC/migration I/O goes through real die-level NAND timing.

VARIANT="${1:?Usage: $0 die_all|die_no2}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ "$VARIANT" != "die_all" && "$VARIANT" != "die_no2" ]]; then
    echo "Error: variant must be 'die_all' or 'die_no2'" >&2
    exit 1
fi

echo "=== Building nvmev_${VARIANT}.ko ==="

# Back up originals
cp ssd.c ssd.c.bak
cp conv_ftl.c conv_ftl.c.bak

# Swap in die-test ssd (GC NAND timing enabled)
cp ssd_die.c ssd.c

# Swap in the right conv_ftl variant
if [[ "$VARIANT" == "die_all" ]]; then
    # conv_ftl.c already has die affinity - use the backup we just made
    cp conv_ftl.c.bak conv_ftl.c
else
    cp baseline_versions/conv_ftl_no2.c conv_ftl.c
fi

# Build
make clean
make APPROACH=on   # DIEAFFINITY=1 for both (not used at runtime, but keeps build consistent)

# Save the output module
cp nvmev.ko "nvmev_${VARIANT}.ko"
echo "=== Built: nvmev_${VARIANT}.ko ==="

# Restore originals
cp ssd.c.bak ssd.c
cp conv_ftl.c.bak conv_ftl.c
rm -f ssd.c.bak conv_ftl.c.bak

echo "=== Original files restored ==="
