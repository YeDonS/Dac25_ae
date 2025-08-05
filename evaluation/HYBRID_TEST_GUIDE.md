# Hybrid SLC/QLC Storage Test Guide

## Overview

This guide explains how to test the NVMeVirt-based hybrid SLC/QLC storage system, including migration effectiveness testing and performance verification.

## Pre-test Preparation

### 1. System Requirements
- Linux kernel version >= 5.0
- At least 16GB RAM (for storage simulation)
- Root privileges

### 2. Install Dependencies
```bash
sudo apt-get update
sudo apt-get install -y fio build-essential linux-headers-$(uname -r) bc
```

### 3. Verify NVMeVirt Module Compilation
```bash
cd ../nvmevirt_DA
make clean
make APPROACH=on
```

## Test Procedures

### Method 1: Quick Test (Recommended)

Use the quick start script to run all tests:

```bash
cd evaluation
sudo ./run_hybrid_test.sh
```

This script will:
1. Check system environment
2. Run hybrid storage-specific tests
3. Ask whether to run the standard test suite

### Method 2: Run Hybrid Storage Tests Separately

If you only want to test hybrid storage functionality:

```bash
cd evaluation
sudo ./test_hybrid_storage.sh
```

### Method 3: Manual Test Steps

1. **Load Module**
   ```bash
   cd ../nvmevirt_DA
   sudo rmmod nvmev 2>/dev/null
   sudo insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3
   ```

2. **Verify Device**
   ```bash
   lsblk | grep nvme
   ```

3. **Run Tests**
   ```bash
   cd ../evaluation
   sudo ./test_hybrid_storage.sh
   ```

## Test Content Description

### 1. Basic Functionality Testing
- Verify device creation and recognition
- Filesystem creation and mounting
- Basic read/write operations

### 2. Migration Effectiveness Testing
- **SLC to QLC Automatic Migration**: Write large amounts of data to trigger migration
- **Hot/Cold Data Separation**: Verify hot data remains in SLC, cold data migrates to QLC
- **Migration Statistics**: Record migration count and patterns

### 3. Performance Comparison Testing
- **SLC Performance**: Test IOPS and latency of high-speed cache layer
- **QLC Performance**: Test IOPS and latency of large capacity layer
- **Performance Comparison**: Calculate SLC/QLC performance differences

### 4. Stress Testing
- Mixed read/write workloads
- Concurrent access testing
- Long-term operation stability

## Viewing Test Results

### Real-time Monitoring
During test execution, monitor from another terminal:

```bash
# View kernel logs
sudo dmesg -w | grep -E "(SLC|QLC|Migration|nvmev)"

# View I/O statistics
iostat -x 1
```

### Analyze Test Results
After test completion, run the analysis script:

```bash
sudo ./analyze_hybrid_results.sh
```

### Result File Locations
All test results are saved in the `./result/hybrid_test_YYYYMMDD_HHMMSS/` directory:

- `test_log.txt` - Complete test logs
- `migration_stats.log` - Migration statistics
- `slc_performance.txt` - SLC layer performance data
- `qlc_performance.txt` - QLC layer performance data
- `stress_test.txt` - Stress test results
- `analysis_summary.txt` - Analysis summary report

## Key Metrics Interpretation

### 1. Migration Effectiveness
- **Normal**: Should see "Migrated LPN xxx from SLC to QLC" logs
- **Migration Count**: Depends on data written, typically > 100 times indicates normal operation

### 2. Performance Differences
- **SLC/QLC IOPS Ratio**: Expected 2-4x
- **Latency Differences**: QLC read latency should be 2-3x that of SLC

### 3. Error Checking
Check if you see the following conditions:
- Migration count is 0
- SLC and QLC performance are identical
- Module loading failed

## Troubleshooting

### 1. Module Loading Failure
```bash
# Check memory reservation
cat /proc/meminfo | grep Hugepages

# Increase memory reservation
echo 5120 > /proc/sys/vm/nr_hugepages
```

### 2. Device Not Appearing
```bash
# Check module status
lsmod | grep nvmev

# View detailed errors
dmesg | tail -50
```

### 3. Performance Anomalies
- Confirm CPU affinity is set correctly
- Check for other I/O loads
- Verify DIEAFFINITY=1 is enabled

## Advanced Testing

### Custom Migration Threshold Testing
Modify parameters in `nvmevirt_DA/ssd_config.h`:
```c
#define MIGRATION_THRESHOLD (10)  // Modify migration threshold
#define SLC_CAPACITY_PERCENT (30) // Adjust SLC proportion
```

Then recompile and test.

### Specific Workload Testing
Add custom fio configurations in `test_hybrid_storage.sh`:
```bash
fio --name=custom_test \
    --ioengine=libaio \
    --rw=randrw \
    --rwmixread=80 \
    --bs=8k \
    --runtime=300 \
    ...
```

## Important Notes

1. Tests require root privileges
2. Tests consume significant memory (10GB+)
3. Recommended to run in dedicated test environment
4. Remember to unload module after testing

## Support

If you encounter issues, please check:
1. `dmesg` logs
2. Log files in the test results directory
3. NVMeVirt project documentation 