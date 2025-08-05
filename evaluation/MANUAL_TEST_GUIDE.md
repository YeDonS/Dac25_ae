# Hybrid SLC/QLC Storage Manual Testing Guide

## Table of Contents
1. [Prerequisites](#prerequisites)
2. [Environment Setup](#environment-setup)
3. [Module Compilation and Loading](#module-compilation-and-loading)
4. [Basic Functionality Testing](#basic-functionality-testing)
5. [Migration Testing](#migration-testing)
6. [Performance Testing](#performance-testing)
7. [Results Analysis](#results-analysis)
8. [Cleanup](#cleanup)
9. [Troubleshooting](#troubleshooting)

## Prerequisites

### System Requirements
- Linux kernel version >= 5.0
- At least 16GB RAM (for storage simulation)
- Root privileges
- CPU with at least 4 cores (recommended)

### Required Tools Installation
```bash
# Update system packages
sudo apt-get update

# Install required tools
sudo apt-get install -y \
    fio \
    build-essential \
    linux-headers-$(uname -r) \
    bc \
    iostat \
    dstat \
    htop

# Verify installation
fio --version
iostat -V
```

### Memory Configuration
```bash
# Check current hugepage configuration
cat /proc/meminfo | grep -i hugepage

# Set hugepages (if needed)
echo 5120 > /proc/sys/vm/nr_hugepages

# Verify hugepage allocation
cat /proc/meminfo | grep -i hugepage
```

## Environment Setup

### 1. Navigate to Project Directory
```bash
cd /Users/junjunya/Desktop/fast24_ae
ls -la  # Verify directory structure
```

### 2. Verify Project Structure
```bash
# Check nvmevirt_DA directory
ls -la nvmevirt_DA/

# Check evaluation directory
ls -la evaluation/

# Expected files in nvmevirt_DA:
# - Makefile
# - ssd_config.h (with hybrid configurations)
# - conv_ftl.c (with migration logic)
# - *.c and *.h files
```

### 3. Check Current System Status
```bash
# Check if nvmev module is already loaded
lsmod | grep nvmev

# If loaded, remove it
sudo rmmod nvmev 2>/dev/null || true

# Check existing NVMe devices
lsblk | grep nvme

# Check system memory
free -h

# Check CPU information
lscpu | grep -E "CPU\(s\)|Model name"
```

## Module Compilation and Loading

### Step 1: Clean Previous Builds
```bash
cd nvmevirt_DA
make clean

# Verify clean
ls -la *.ko *.o 2>/dev/null || echo "Clean successful"
```

### Step 2: Compile with Hybrid Support
```bash
# Compile with APPROACH=on for hybrid storage
make APPROACH=on

# Expected output should show successful compilation
# Look for: "BUILD SUCCESSFUL" or similar message

# Verify compiled module exists
ls -la nvmev_on.ko
file nvmev_on.ko  # Should show "ELF 64-bit LSB relocatable"
```

### Step 3: Load the Module
```bash
# Load module with specific parameters
sudo insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# Check module loading
lsmod | grep nvmev

# Expected output:
# nvmev                 XXXXX  0
```

### Step 4: Verify Device Creation
```bash
# Wait for device initialization
sleep 3

# Check for new NVMe device
lsblk | grep nvme
# Expected: nvme0n1 or similar device

# Check device details
sudo fdisk -l /dev/nvme0n1

# Check kernel messages
dmesg | tail -20 | grep -E "(nvmev|SLC|QLC)"
# Expected messages about SLC/QLC initialization
```

## Basic Functionality Testing

### Step 1: Create Filesystem
```bash
cd ../evaluation

# Create ext4 filesystem on the hybrid device
sudo mkfs.ext4 -F /dev/nvme0n1

# Expected output: Filesystem created successfully
```

### Step 2: Mount Device
```bash
# Create mount point
sudo mkdir -p /mnt/test_hybrid

# Mount the device
sudo mount /dev/nvme0n1 /mnt/test_hybrid

# Verify mount
mount | grep nvme0n1
df -h /mnt/test_hybrid
```

### Step 3: Basic Write Test
```bash
# Create test directory
sudo mkdir -p /mnt/test_hybrid/basic_tests
cd /mnt/test_hybrid/basic_tests

# Basic write test (should go to SLC)
echo "Starting basic write test..."
sudo dd if=/dev/zero of=/mnt/test_hybrid/basic_tests/test1.dat bs=4K count=1000 oflag=direct

# Check dmesg for SLC write messages
dmesg | tail -10 | grep -E "(SLC|write|nvmev)"

# Expected: Messages about SLC writes
```

### Step 4: Basic Read Test
```bash
# Basic read test
echo "Starting basic read test..."
sudo dd if=/mnt/test_hybrid/basic_tests/test1.dat of=/dev/null bs=4K count=1000 iflag=direct

# Verify file integrity
ls -la /mnt/test_hybrid/basic_tests/
```

## Migration Testing

### Step 1: Generate Data for Migration
```bash
echo "=== Migration Testing Phase ==="

# Create large files to trigger SLC->QLC migration
for i in {1..5}; do
    echo "Creating file batch $i..."
    sudo dd if=/dev/zero of=/mnt/test_hybrid/migration_test_$i.dat bs=1M count=200 oflag=direct
    sync
    sleep 2
    
    # Check for migration messages
    dmesg | grep -E "(Migration|Migrated)" | tail -5
done
```

### Step 2: Monitor Migration Process
```bash
# Monitor migration in real-time (run in separate terminal)
# Terminal 1:
sudo dmesg -w | grep -E "(Migration|Migrated|SLC|QLC)"

# Terminal 2 (continue with test):
# Force more writes to trigger migration
for i in {6..10}; do
    echo "Creating additional file $i to force migration..."
    sudo dd if=/dev/zero of=/mnt/test_hybrid/migration_test_$i.dat bs=1M count=150 oflag=direct
    sync
    sleep 3
done
```

### Step 3: Verify Migration Statistics
```bash
# Check migration statistics
dmesg | grep "Write Stats" | tail -1

# Expected output format:
# [nvmev] Write Stats: SLC writes=XXXX, QLC writes=YYYY, Migrations=ZZZZ

# Count total migrations
MIGRATION_COUNT=$(dmesg | grep -c "Migrated LPN")
echo "Total migrations detected: $MIGRATION_COUNT"

# Migration count should be > 0, ideally > 50 for this test
```

### Step 4: Hot/Cold Data Pattern Testing
```bash
echo "=== Hot/Cold Data Pattern Testing ==="

# Create hot data (frequently accessed)
echo "Creating hot data..."
for i in {1..50}; do
    echo "Hot data iteration $i - $(date)" >> /mnt/test_hybrid/hot_data.txt
    sync
done

# Create cold data (write once, don't access)
echo "Creating cold data..."
sudo dd if=/dev/zero of=/mnt/test_hybrid/cold_data.dat bs=1M count=100 oflag=direct
sync

# Wait for potential migration
sleep 10

# Access hot data multiple times
echo "Re-accessing hot data to keep it hot..."
for i in {1..20}; do
    cat /mnt/test_hybrid/hot_data.txt > /dev/null
    sleep 1
done

# Check access patterns in dmesg
dmesg | grep -E "(hot|cold|access)" -i | tail -10
```

## Performance Testing

### Step 1: SLC Performance Testing
```bash
echo "=== SLC Performance Testing ==="

# Test SLC write performance (new data goes to SLC)
echo "Testing SLC write performance..."
fio --name=slc_write_test \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --direct=1 \
    --size=50M \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --filename=/mnt/test_hybrid/slc_perf.dat \
    --output-format=normal

# Record results
fio --name=slc_write_test \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --direct=1 \
    --size=50M \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --filename=/mnt/test_hybrid/slc_perf.dat \
    --output=/tmp/slc_performance.txt
```

### Step 2: QLC Performance Testing
```bash
echo "=== QLC Performance Testing ==="

# Wait for some data to migrate to QLC
echo "Waiting for migration to QLC..."
sleep 60

# Test QLC read performance (reading potentially migrated data)
echo "Testing QLC read performance..."
fio --name=qlc_read_test \
    --ioengine=libaio \
    --rw=randread \
    --bs=4k \
    --direct=1 \
    --size=50M \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --filename=/mnt/test_hybrid/cold_data.dat \
    --output=/tmp/qlc_performance.txt \
    --output-format=normal
```

### Step 3: Mixed Workload Testing
```bash
echo "=== Mixed Workload Testing ==="

# Run mixed read/write workload
fio --name=mixed_test \
    --ioengine=libaio \
    --rw=randrw \
    --rwmixread=70 \
    --bs=4k \
    --direct=1 \
    --size=100M \
    --numjobs=2 \
    --runtime=60 \
    --time_based \
    --filename=/mnt/test_hybrid/mixed_test.dat \
    --output=/tmp/mixed_performance.txt \
    --output-format=normal
```

### Step 4: Monitor System During Tests
```bash
# In separate terminals, monitor:

# Terminal 1: I/O statistics
iostat -x 1

# Terminal 2: System resources
htop

# Terminal 3: Kernel messages
dmesg -w | grep -E "(nvmev|SLC|QLC|Migration)"

# Terminal 4: Block device statistics
while true; do
    cat /proc/diskstats | grep nvme
    sleep 2
done
```

## Results Analysis

### Step 1: Collect All Logs
```bash
# Create results directory
mkdir -p /tmp/manual_test_results_$(date +%Y%m%d_%H%M%S)
RESULT_DIR="/tmp/manual_test_results_$(date +%Y%m%d_%H%M%S)"

# Collect dmesg logs
dmesg > $RESULT_DIR/dmesg_full.log
dmesg | grep -E "(SLC|QLC|Migration|nvmev)" > $RESULT_DIR/hybrid_messages.log

# Copy performance results
cp /tmp/*_performance.txt $RESULT_DIR/ 2>/dev/null || true
```

### Step 2: Analyze Migration Effectiveness
```bash
# Count migrations
TOTAL_MIGRATIONS=$(grep -c "Migrated LPN" $RESULT_DIR/hybrid_messages.log)
echo "Total migrations: $TOTAL_MIGRATIONS"

# Show migration statistics
grep "Write Stats" $RESULT_DIR/hybrid_messages.log | tail -1

# Show sample migration messages
echo "Sample migration messages:"
grep "Migrated LPN" $RESULT_DIR/hybrid_messages.log | head -5
```

### Step 3: Analyze Performance Results
```bash
# Extract SLC performance
if [ -f "$RESULT_DIR/slc_performance.txt" ]; then
    echo "=== SLC Performance ==="
    grep -E "(iops|bw=|lat.*avg)" $RESULT_DIR/slc_performance.txt | head -5
fi

# Extract QLC performance
if [ -f "$RESULT_DIR/qlc_performance.txt" ]; then
    echo "=== QLC Performance ==="
    grep -E "(iops|bw=|lat.*avg)" $RESULT_DIR/qlc_performance.txt | head -5
fi

# Compare performance
echo "=== Performance Comparison ==="
SLC_IOPS=$(grep "iops" $RESULT_DIR/slc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2)
QLC_IOPS=$(grep "iops" $RESULT_DIR/qlc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2)

if [ ! -z "$SLC_IOPS" ] && [ ! -z "$QLC_IOPS" ]; then
    RATIO=$(echo "scale=2; $SLC_IOPS / $QLC_IOPS" | bc 2>/dev/null)
    echo "SLC IOPS: $SLC_IOPS"
    echo "QLC IOPS: $QLC_IOPS"
    echo "Performance Ratio (SLC/QLC): ${RATIO}x"
fi
```

### Step 4: Generate Summary Report
```bash
# Create comprehensive summary
cat > $RESULT_DIR/manual_test_summary.txt <<EOF
Hybrid SLC/QLC Storage Manual Test Summary
Generated: $(date)
Test Duration: Manual test execution

=== System Information ===
Kernel: $(uname -r)
CPU: $(lscpu | grep "Model name" | cut -d: -f2 | xargs)
Memory: $(free -h | grep "Mem:" | awk '{print $2}')

=== Migration Results ===
Total Migrations: $TOTAL_MIGRATIONS
Migration Status: $([ "$TOTAL_MIGRATIONS" -gt 0 ] && echo "WORKING" || echo "NOT DETECTED")

=== Performance Results ===
SLC IOPS: ${SLC_IOPS:-N/A}
QLC IOPS: ${QLC_IOPS:-N/A}
Performance Ratio: ${RATIO:-N/A}x

=== Test Status ===
Basic Functionality: $([ -f "/mnt/test_hybrid/basic_tests/test1.dat" ] && echo "PASSED" || echo "FAILED")
Migration Testing: $([ "$TOTAL_MIGRATIONS" -gt 0 ] && echo "PASSED" || echo "FAILED")
Performance Testing: $([ ! -z "$SLC_IOPS" ] && echo "PASSED" || echo "FAILED")

=== Recommendations ===
$([ "$TOTAL_MIGRATIONS" -eq 0 ] && echo "- Check migration threshold settings in ssd_config.h")
$([ -z "$RATIO" ] && echo "- Verify performance test execution and fio output")
$([ -z "$SLC_IOPS" ] && echo "- Check fio configuration and output files")

=== Log Files Location ===
All detailed logs saved in: $RESULT_DIR
EOF

# Display summary
cat $RESULT_DIR/manual_test_summary.txt
echo ""
echo "Detailed results saved in: $RESULT_DIR"
```

## Cleanup

### Step 1: Unmount and Remove Test Data
```bash
# Unmount filesystem
sudo umount /mnt/test_hybrid

# Remove mount point
sudo rmdir /mnt/test_hybrid

# Verify unmount
mount | grep nvme || echo "Successfully unmounted"
```

### Step 2: Unload Module
```bash
# Remove the nvmev module
sudo rmmod nvmev

# Verify removal
lsmod | grep nvmev || echo "Module successfully removed"

# Check no remaining NVMe devices from nvmev
lsblk | grep nvme || echo "No nvmev devices remaining"
```

### Step 3: Clean Temporary Files
```bash
# Remove temporary performance files
rm -f /tmp/*_performance.txt

# Optional: Clean hugepages (if you want to restore original settings)
# echo 0 > /proc/sys/vm/nr_hugepages
```

## Troubleshooting

### Module Loading Issues

**Problem**: Module fails to load
```bash
# Check detailed error
dmesg | tail -20

# Common solutions:
# 1. Check kernel headers
ls /lib/modules/$(uname -r)/build || sudo apt-get install linux-headers-$(uname -r)

# 2. Check memory
free -h
# Ensure at least 16GB available

# 3. Recompile module
make clean && make APPROACH=on
```

### Device Not Appearing

**Problem**: No NVMe device created after module load
```bash
# Check module parameters
modinfo nvmev_on.ko

# Try different parameters
sudo rmmod nvmev
sudo insmod nvmev_on.ko memmap_start=8G memmap_size=8G cpus=0,1

# Check system messages
dmesg | grep -E "(nvmev|error|fail)" | tail -10
```

### Migration Not Working

**Problem**: No migration messages in dmesg
```bash
# Check migration threshold
grep MIGRATION_THRESHOLD ../nvmevirt_DA/ssd_config.h

# Write more data to trigger migration
for i in {1..20}; do
    sudo dd if=/dev/zero of=/mnt/test_hybrid/force_migration_$i.dat bs=1M count=100 oflag=direct
    sync
    sleep 2
done

# Check SLC capacity settings
grep SLC_CAPACITY_PERCENT ../nvmevirt_DA/ssd_config.h
```

### Performance Issues

**Problem**: No performance difference between SLC and QLC
```bash
# Check compilation flags
grep -n QLC_PROG_LATENCY ../nvmevirt_DA/ssd_config.h

# Verify fio is using direct I/O
fio --name=test --direct=1 --ioengine=libaio --rw=read --bs=4k --size=1M --filename=/mnt/test_hybrid/test.dat

# Check system not under load
htop  # Ensure low CPU usage during tests
```

### Memory Issues

**Problem**: Insufficient memory errors
```bash
# Check available memory
free -h

# Increase hugepages
echo 8192 > /proc/sys/vm/nr_hugepages

# Use smaller memory allocation
sudo rmmod nvmev
sudo insmod nvmev_on.ko memmap_start=8G memmap_size=4G cpus=0,1
```

## Expected Results Summary

### Successful Test Indicators
1. **Module Loading**: `lsmod | grep nvmev` shows module loaded
2. **Device Creation**: `lsblk` shows nvme device
3. **SLC Messages**: dmesg shows "SLC writes" messages
4. **Migration Messages**: dmesg shows "Migrated LPN" messages (>50 for full test)
5. **Performance Difference**: SLC performance 2-4x better than QLC
6. **No Errors**: No error messages in dmesg related to nvmev

### Performance Expectations
- **SLC Write IOPS**: ~15,000-30,000 IOPS
- **QLC Read IOPS**: ~5,000-15,000 IOPS
- **Latency Ratio**: QLC latency should be 2-3x higher than SLC
- **Migration Count**: >100 migrations for comprehensive test

### File Verification
After testing, verify these files exist:
- `/tmp/manual_test_results_*/manual_test_summary.txt`
- `/tmp/manual_test_results_*/hybrid_messages.log`
- `/tmp/manual_test_results_*/*_performance.txt`

This completes the detailed manual testing procedure for the hybrid SLC/QLC storage system. 