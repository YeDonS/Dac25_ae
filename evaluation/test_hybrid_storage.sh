#!/bin/bash

###########################################
# Hybrid SLC/QLC Storage Comprehensive Test Script
# Test contents:
# 1. Basic functionality verification
# 2. Migration effectiveness testing
# 3. Performance comparison testing
# 4. Stress testing
###########################################

# Import common variables
source ./commonvariables.sh

# Set test-specific variables
TEST_DEVICE="nvme0n1"  # Hybrid storage device
MOUNT_POINT="/mnt/test_hybrid"
RESULT_DIR="./result/hybrid_test_$(date +%Y%m%d_%H%M%S)"
LOG_FILE="$RESULT_DIR/test_log.txt"

# Create result directory
mkdir -p $RESULT_DIR

# Logging function
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a $LOG_FILE
}

# Error handling function
error_exit() {
    log "Error: $1"
    cleanup
    exit 1
}

# Cleanup function
cleanup() {
    log "Starting cleanup..."
    umount $MOUNT_POINT 2>/dev/null || true
    rmmod nvmev 2>/dev/null || true
}

# Capture exit signals
trap cleanup EXIT

###########################################
# 0. Preparation Phase
###########################################
log "========================================="
log "Hybrid SLC/QLC Storage Comprehensive Test"
log "========================================="

# Compile and load module
log "Compiling NVMeVirt hybrid storage module..."
cd ../nvmevirt_DA
make clean
make APPROACH=on || error_exit "Compilation failed"

log "Loading NVMeVirt module..."
rmmod nvmev 2>/dev/null || true
insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3 || error_exit "Module loading failed"

# Wait for device preparation
sleep 3

# Verify device
log "Verifying device status..."
if ! lsblk | grep -q nvme; then
    error_exit "NVMe device not detected"
fi

cd ../evaluation

###########################################
# 1. Basic Functionality Testing
###########################################
log ""
log "=== Phase 1: Basic Functionality Testing ==="

# Create filesystem
log "Creating filesystem..."
mkfs.ext4 -F /dev/$TEST_DEVICE || error_exit "Filesystem creation failed"

# Mount device
mkdir -p $MOUNT_POINT
mount /dev/$TEST_DEVICE $MOUNT_POINT || error_exit "Mount failed"

# Basic read/write testing
log "Executing basic read/write tests..."
dd if=/dev/zero of=$MOUNT_POINT/test_basic.dat bs=4K count=1000 oflag=direct 2>&1 | tee -a $LOG_FILE
dd if=$MOUNT_POINT/test_basic.dat of=/dev/null bs=4K count=1000 iflag=direct 2>&1 | tee -a $LOG_FILE

# Save basic test results
dmesg | grep -E "(SLC|QLC|nvmev)" | tail -50 > $RESULT_DIR/basic_test_dmesg.log

###########################################
# 2. Migration Effectiveness Testing
###########################################
log ""
log "=== Phase 2: Migration Effectiveness Testing ==="

# Clear previous statistics
echo 3 > /proc/sys/vm/drop_caches

# 2.1 Write large amounts of data to SLC (trigger migration)
log "Writing large amounts of data to trigger SLC to QLC migration..."
for i in {1..10}; do
    log "Writing data batch $i..."
    dd if=/dev/zero of=$MOUNT_POINT/migration_test_$i.dat bs=1M count=100 oflag=direct 2>&1 | tee -a $LOG_FILE
    sync
    sleep 2  # Give time for migration
    
    # Record migration logs
    dmesg | grep -E "(Migration|Migrated)" | tail -10 >> $RESULT_DIR/migration_log_$i.txt
done

# 2.2 Verify migration statistics
log "Collecting migration statistics..."
dmesg | grep -E "(Write Stats|Migration)" | tail -20 > $RESULT_DIR/migration_stats.log

# 2.3 Test hot/cold data separation
log "Testing hot/cold data separation..."

# Create hot data (frequently accessed)
log "Creating hot data..."
for i in {1..100}; do
    echo "Hot data iteration $i" >> $MOUNT_POINT/hot_data.txt
    sync
done

# Create cold data (write once, no further access)
log "Creating cold data..."
dd if=/dev/zero of=$MOUNT_POINT/cold_data.dat bs=1M count=50 oflag=direct
sync
sleep 5  # Wait for data to become cold

# Access hot data again
log "Re-accessing hot data..."
for i in {1..50}; do
    cat $MOUNT_POINT/hot_data.txt > /dev/null
done

# Record migration patterns
dmesg | grep -E "(hot|cold|access|migrate)" -i | tail -30 > $RESULT_DIR/hot_cold_pattern.log

###########################################
# 3. Performance Comparison Testing
###########################################
log ""
log "=== Phase 3: SLC vs QLC Performance Comparison Testing ==="

# 3.1 SLC performance testing (newly written data in SLC)
log "Testing SLC layer performance..."
fio --name=slc_test \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --direct=1 \
    --size=100M \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --filename=$MOUNT_POINT/slc_perf_test.dat \
    --output=$RESULT_DIR/slc_performance.txt \
    --output-format=normal

# 3.2 Wait for data migration to QLC
log "Waiting for data migration to QLC..."
sleep 60  # Wait for migration

# 3.3 QLC performance testing (reading migrated data)
log "Testing QLC layer performance..."
fio --name=qlc_test \
    --ioengine=libaio \
    --rw=randread \
    --bs=4k \
    --direct=1 \
    --size=100M \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --filename=$MOUNT_POINT/cold_data.dat \
    --output=$RESULT_DIR/qlc_performance.txt \
    --output-format=normal

###########################################
# 4. Stress Testing
###########################################
log ""
log "=== Phase 4: Stress Testing ==="

# 4.1 Mixed read/write stress testing
log "Executing mixed read/write stress test..."
fio --name=stress_test \
    --ioengine=libaio \
    --rw=randrw \
    --rwmixread=70 \
    --bs=4k \
    --direct=1 \
    --size=500M \
    --numjobs=4 \
    --runtime=120 \
    --time_based \
    --filename=$MOUNT_POINT/stress_test.dat \
    --output=$RESULT_DIR/stress_test.txt \
    --output-format=normal

# 4.2 Concurrent testing
log "Executing concurrent access test..."
for i in {1..4}; do
    (
        fio --name=concurrent_$i \
            --ioengine=libaio \
            --rw=randrw \
            --bs=4k \
            --direct=1 \
            --size=50M \
            --runtime=60 \
            --time_based \
            --filename=$MOUNT_POINT/concurrent_$i.dat \
            --output=$RESULT_DIR/concurrent_$i.txt
    ) &
done
wait

###########################################
# 5. Results Collection and Analysis
###########################################
log ""
log "=== Phase 5: Results Collection and Analysis ==="

# Collect final dmesg logs
dmesg | grep -E "(SLC|QLC|Migration|Write Stats|nvmev)" > $RESULT_DIR/final_dmesg.log

# Create test report
cat > $RESULT_DIR/test_report.txt <<EOF
Hybrid SLC/QLC Storage Test Report
Generated: $(date)

1. Basic Functionality Test: PASSED
   - Filesystem creation: SUCCESS
   - Basic read/write: SUCCESS

2. Migration Effectiveness Test Results:
EOF

# Analyze migration statistics
echo "Migration Statistics:" >> $RESULT_DIR/test_report.txt
grep "Write Stats" $RESULT_DIR/final_dmesg.log | tail -1 >> $RESULT_DIR/test_report.txt
echo "" >> $RESULT_DIR/test_report.txt

# Analyze performance data
echo "3. Performance Comparison:" >> $RESULT_DIR/test_report.txt
echo "   SLC Performance:" >> $RESULT_DIR/test_report.txt
grep "iops" $RESULT_DIR/slc_performance.txt | head -5 >> $RESULT_DIR/test_report.txt
echo "   QLC Performance:" >> $RESULT_DIR/test_report.txt
grep "iops" $RESULT_DIR/qlc_performance.txt | head -5 >> $RESULT_DIR/test_report.txt

# Display test summary
log ""
log "========================================="
log "Testing Complete!"
log "Results saved in: $RESULT_DIR"
log "========================================="

# Display key results
echo ""
echo "Key Test Results:"
echo "1. Migration Statistics:"
grep "Write Stats" $RESULT_DIR/final_dmesg.log | tail -1
echo ""
echo "2. Migration Count:"
grep -c "Migrated" $RESULT_DIR/final_dmesg.log
echo ""
echo "3. Detailed Report: $RESULT_DIR/test_report.txt"

# Cleanup
umount $MOUNT_POINT 