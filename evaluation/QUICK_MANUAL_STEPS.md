# Quick Manual Testing Steps Reference

## 🚀 Essential Commands Quick Reference

### Phase 1: Setup and Compilation
```bash
# 1. Navigate to project
cd /Users/junjunya/Desktop/fast24_ae

# 2. Clean and compile
cd nvmevirt_DA
make clean
make APPROACH=on

# 3. Load module
sudo rmmod nvmev 2>/dev/null || true
sudo insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# 4. Verify device
sleep 3
lsblk | grep nvme
```

### Phase 2: Basic Testing
```bash
# 5. Setup filesystem
cd ../evaluation
sudo mkfs.ext4 -F /dev/nvme0n1
sudo mkdir -p /mnt/test_hybrid
sudo mount /dev/nvme0n1 /mnt/test_hybrid

# 6. Basic write test (goes to SLC)
sudo dd if=/dev/zero of=/mnt/test_hybrid/basic_test.dat bs=4K count=1000 oflag=direct

# 7. Check for SLC messages
dmesg | grep -E "(SLC|nvmev)" | tail -10
```

### Phase 3: Migration Testing
```bash
# 8. Generate large files to trigger migration
for i in {1..8}; do
    echo "Creating migration file $i..."
    sudo dd if=/dev/zero of=/mnt/test_hybrid/migration_$i.dat bs=1M count=150 oflag=direct
    sync
    sleep 2
done

# 9. Check migration messages
dmesg | grep -E "(Migration|Migrated)" | tail -10
MIGRATION_COUNT=$(dmesg | grep -c "Migrated LPN")
echo "Total migrations: $MIGRATION_COUNT"

# 10. Get migration statistics
dmesg | grep "Write Stats" | tail -1
```

### Phase 4: Performance Testing
```bash
# 11. Test SLC performance (write new data)
fio --name=slc_test --ioengine=libaio --rw=randwrite --bs=4k --direct=1 \
    --size=50M --runtime=30 --time_based --filename=/mnt/test_hybrid/slc_test.dat \
    --output=/tmp/slc_performance.txt

# 12. Wait and test QLC performance (read old data)
sleep 30
fio --name=qlc_test --ioengine=libaio --rw=randread --bs=4k --direct=1 \
    --size=50M --runtime=30 --time_based --filename=/mnt/test_hybrid/migration_1.dat \
    --output=/tmp/qlc_performance.txt

# 13. Compare performance
echo "=== Performance Comparison ==="
SLC_IOPS=$(grep "iops" /tmp/slc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2)
QLC_IOPS=$(grep "iops" /tmp/qlc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2)
echo "SLC IOPS: $SLC_IOPS"
echo "QLC IOPS: $QLC_IOPS"
if [ ! -z "$SLC_IOPS" ] && [ ! -z "$QLC_IOPS" ]; then
    RATIO=$(echo "scale=2; $SLC_IOPS / $QLC_IOPS" | bc)
    echo "Performance Ratio: ${RATIO}x"
fi
```

### Phase 5: Results Analysis
```bash
# 14. Create results directory
RESULT_DIR="/tmp/manual_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULT_DIR

# 15. Collect logs
dmesg | grep -E "(SLC|QLC|Migration|nvmev)" > $RESULT_DIR/hybrid_messages.log
cp /tmp/*_performance.txt $RESULT_DIR/ 2>/dev/null

# 16. Generate summary
echo "=== Test Summary ===" > $RESULT_DIR/summary.txt
echo "Date: $(date)" >> $RESULT_DIR/summary.txt
echo "Migration Count: $(grep -c 'Migrated LPN' $RESULT_DIR/hybrid_messages.log)" >> $RESULT_DIR/summary.txt
echo "SLC IOPS: $SLC_IOPS" >> $RESULT_DIR/summary.txt
echo "QLC IOPS: $QLC_IOPS" >> $RESULT_DIR/summary.txt
echo "Performance Ratio: ${RATIO:-N/A}x" >> $RESULT_DIR/summary.txt

cat $RESULT_DIR/summary.txt
echo "Detailed results in: $RESULT_DIR"
```

### Phase 6: Cleanup
```bash
# 17. Unmount and cleanup
sudo umount /mnt/test_hybrid
sudo rmdir /mnt/test_hybrid
sudo rmmod nvmev
lsmod | grep nvmev || echo "Module successfully removed"
```

## 🔍 Monitoring Commands (Run in Separate Terminals)

### Terminal 1: Real-time Migration Monitoring
```bash
sudo dmesg -w | grep -E "(Migration|Migrated|SLC|QLC)"
```

### Terminal 2: System Resource Monitoring
```bash
# Monitor I/O
iostat -x 2

# Or monitor overall system
htop
```

### Terminal 3: Progress Checker
```bash
# Use our progress checker
sudo ./manual_test_checker.sh

# Or check specific aspects
sudo ./manual_test_checker.sh migration
sudo ./manual_test_checker.sh perf
```

## ✅ Success Criteria Checklist

- [ ] Module loads without errors: `lsmod | grep nvmev`
- [ ] NVMe device appears: `lsblk | grep nvme`
- [ ] Filesystem mounts successfully: `mount | grep nvme`
- [ ] SLC messages in dmesg: `dmesg | grep SLC | wc -l` > 0
- [ ] Migration messages appear: `dmesg | grep "Migrated LPN" | wc -l` > 50
- [ ] Performance difference: SLC/QLC ratio >= 2.0x
- [ ] No error messages: `dmesg | grep -i error | grep nvmev | wc -l` = 0

## 🚨 Quick Troubleshooting

### Module Won't Load
```bash
# Check memory
free -h  # Need ~16GB available
# Set hugepages if needed
echo 5120 > /proc/sys/vm/nr_hugepages
```

### No Migration Messages
```bash
# Write more data
for i in {1..15}; do
    sudo dd if=/dev/zero of=/mnt/test_hybrid/force_$i.dat bs=1M count=100 oflag=direct
    sync
done
```

### No Performance Difference
```bash
# Check if QLC latency is configured
grep QLC_PROG_LATENCY ../nvmevirt_DA/ssd_config.h
# Should show: #define QLC_PROG_LATENCY (185000 * 4)
```

## 📊 Expected Results

| Metric | Expected Value | Command to Check |
|--------|---------------|------------------|
| Module Loading | Success | `lsmod \| grep nvmev` |
| Device Creation | nvme0n1 appears | `lsblk \| grep nvme` |
| Migration Count | > 50 migrations | `dmesg \| grep -c "Migrated LPN"` |
| SLC IOPS | 15,000-30,000 | `grep iops /tmp/slc_performance.txt` |
| QLC IOPS | 5,000-15,000 | `grep iops /tmp/qlc_performance.txt` |
| Performance Ratio | 2.0x - 4.0x | Calculate SLC_IOPS/QLC_IOPS |

## 📝 Key Log Messages to Look For

### Successful Initialization
```
[nvmev] SLC blocks per plane: 1638 (20%), QLC blocks per plane: 6554 (80%)
[nvmev] Hybrid SLC/QLC storage initialized
```

### Migration Activity
```
[nvmev] Migrated LPN 12345 from SLC to QLC region 2
[nvmev] Write Stats: SLC writes=5000, QLC writes=100, Migrations=200
```

### Performance Indicators
```
# In fio output:
write: IOPS=25.5k, BW=99.7MiB/s  # SLC performance
read: IOPS=8.2k, BW=32.1MiB/s    # QLC performance
```

---

**Total Test Time**: ~30-45 minutes  
**Key Files**: MANUAL_TEST_GUIDE.md (detailed), manual_test_checker.sh (progress checking) 