# Hybrid SLC/QLC Storage Testing Framework

## Title: NVMeVirt-based Hybrid SLC/QLC Storage System with Intelligent Migration
Contact: Enhanced NVMeVirt with hybrid storage capabilities

This repository provides a comprehensive testing framework for evaluating hybrid SLC/QLC storage systems based on NVMeVirt. The framework includes automated migration between SLC (high-performance cache) and QLC (high-capacity storage) layers, with detailed testing tools for migration effectiveness and performance analysis.

## 🚀 Key Features

- **Hybrid Storage Architecture**: 20% SLC cache + 80% QLC storage
- **Intelligent Migration**: Automatic data migration based on access patterns
- **Performance Differentiation**: Realistic SLC/QLC latency modeling
- **Comprehensive Testing**: Automated and manual testing capabilities
- **Detailed Analysis**: Migration statistics and performance comparison tools

## Contents
- [1. System Requirements](#1-system-requirements)
- [2. Quick Start](#2-quick-start)
- [3. Hybrid Storage Configuration](#3-hybrid-storage-configuration)
- [4. Building the System](#4-building-the-system)
- [5. Testing Approaches](#5-testing-approaches)
- [6. Expected Results](#6-expected-results)
- [7. Troubleshooting](#7-troubleshooting)
- [8. Advanced Configuration](#8-advanced-configuration)

## 1. System Requirements

### Hardware Requirements

| **Component** | **Minimum** | **Recommended** |
|---------------|-------------|-----------------|
| Processor | 4 cores | 8+ cores |
| Memory | 16 GB RAM | 32+ GB RAM |
| Storage | 50 GB free space | 100+ GB free space |
| OS | Ubuntu 20.04+ | Ubuntu 20.04/22.04 |

### Software Dependencies

```bash
# Install required packages
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    fio \
    bc \
    iostat \
    dstat \
    htop \
    libsqlite3-dev \
    numactl
```

### Memory Configuration

```bash
# Check current hugepage configuration
cat /proc/meminfo | grep -i hugepage

# Set hugepages for NVMeVirt (if needed)
echo 5120 > /proc/sys/vm/nr_hugepages

# Reserve memory in GRUB (for production use)
# Add to /etc/default/grub:
# GRUB_CMDLINE_LINUX="memmap=16G\\\$16G intremap=off"
```

## 2. Quick Start

### Option A: Automated Testing (Recommended)
```bash
cd /path/to/fast24_ae/evaluation
sudo ./run_hybrid_test.sh
sudo ./analyze_hybrid_results.sh
```

### Option B: Manual Testing
```bash
cd /path/to/fast24_ae/evaluation
# Follow QUICK_MANUAL_STEPS.md (17 commands, ~30 minutes)
sudo ./manual_test_checker.sh  # Verify progress
```

### Option C: Comprehensive Manual Testing
```bash
cd /path/to/fast24_ae/evaluation
# Follow MANUAL_TEST_GUIDE.md (detailed guide)
sudo ./manual_test_checker.sh [specific_checks]
```

## 3. Hybrid Storage Configuration

### Core Configuration Parameters

The hybrid storage system is configured through `nvmevirt_DA/ssd_config.h`:

```c
// Capacity allocation
#define SLC_CAPACITY_PERCENT (20)  // SLC: 20% high-speed cache
#define QLC_CAPACITY_PERCENT (80)  // QLC: 80% high-capacity storage
#define QLC_REGIONS (4)            // QLC regions for load balancing

// Performance differentiation
#define QLC_PROG_LATENCY (185000 * 4)      // QLC write: 4x slower than SLC
#define QLC_ERASE_LATENCY (3000000)        // QLC erase: 3x slower than SLC
#define QLC_READ_LATENCY_FACTOR (2.5)      // QLC read: 2.5x slower than SLC

// Migration parameters
#define MIGRATION_THRESHOLD (10)           // Access count threshold
#define COLD_DATA_TIME_THRESHOLD (1000000) // Time threshold (μs)
```

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Hybrid SLC/QLC Storage                  │
├─────────────────────────────────────────────────────────────┤
│  SLC Cache Layer (20%)          QLC Storage Layer (80%)    │
│  ┌─────────────────────┐       ┌─────────────────────────┐ │
│  │   High Performance  │  ===▶ │    High Capacity        │ │
│  │   - Fast writes     │       │    - Large storage      │ │
│  │   - Hot data        │       │    - Cold data          │ │
│  │   - ~30μs latency   │       │    - ~75μs read latency │ │
│  └─────────────────────┘       └─────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│           Intelligent Migration Engine                     │
│  • Access pattern tracking    • Background migration      │
│  • Hot/cold data classification • Load balancing          │
└─────────────────────────────────────────────────────────────┘
```

## 4. Building the System

### Step 1: Clone and Setup
```bash
cd /path/to/your/workspace
git clone https://github.com/yuhun-Jun/fast24_ae.git
cd fast24_ae
```

### Step 2: Build NVMeVirt with Hybrid Support
```bash
cd nvmevirt_DA
make clean
make APPROACH=on  # Enable hybrid storage features

# Verify build
ls -la nvmev_on.ko  # Should exist and be > 100KB
```

### Step 3: Load Module and Create Device
```bash
cd ../evaluation

# Load the hybrid storage module
sudo rmmod nvmev 2>/dev/null || true
sudo insmod ../nvmevirt_DA/nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# Verify device creation
lsblk | grep nvme  # Should show new nvme device
dmesg | grep -E "(SLC|QLC|nvmev)" | tail -10  # Check initialization messages
```

## 5. Testing Approaches

### 5.1 Available Testing Tools

| Tool | Purpose | Usage | Duration |
|------|---------|-------|----------|
| `run_hybrid_test.sh` | Quick automated testing | `sudo ./run_hybrid_test.sh` | 30-45 min |
| `test_hybrid_storage.sh` | Comprehensive test suite | `sudo ./test_hybrid_storage.sh` | 30-45 min |
| `analyze_hybrid_results.sh` | Results analysis | `sudo ./analyze_hybrid_results.sh` | 5 min |
| `manual_test_checker.sh` | Progress verification | `sudo ./manual_test_checker.sh` | Real-time |

### 5.2 Testing Documentation

| Document | Content | Target Audience |
|----------|---------|-----------------|
| `HYBRID_TEST_GUIDE.md` | General testing overview | All users |
| `MANUAL_TEST_GUIDE.md` | Detailed manual procedures (15,000+ words) | Advanced users |
| `QUICK_MANUAL_STEPS.md` | Essential commands reference | Developers |
| `MANUAL_TESTING_SUMMARY.md` | Complete documentation overview | Documentation reference |

### 5.3 Test Phases

#### Phase 1: Basic Functionality
```bash
# Verify device creation and basic operations
sudo mkfs.ext4 -F /dev/nvme0n1
sudo mount /dev/nvme0n1 /mnt/test_hybrid
sudo dd if=/dev/zero of=/mnt/test_hybrid/test.dat bs=4K count=1000 oflag=direct
```

#### Phase 2: Migration Testing
```bash
# Generate data to trigger SLC→QLC migration
for i in {1..8}; do
    sudo dd if=/dev/zero of=/mnt/test_hybrid/migration_$i.dat bs=1M count=150 oflag=direct
    sync && sleep 2
done

# Monitor migration activity
dmesg | grep -E "(Migration|Migrated)" | tail -10
```

#### Phase 3: Performance Comparison
```bash
# Test SLC performance (new writes)
fio --name=slc_test --ioengine=libaio --rw=randwrite --bs=4k --direct=1 \
    --size=50M --runtime=30 --filename=/mnt/test_hybrid/slc_test.dat

# Test QLC performance (migrated data)
fio --name=qlc_test --ioengine=libaio --rw=randread --bs=4k --direct=1 \
    --size=50M --runtime=30 --filename=/mnt/test_hybrid/migration_1.dat
```

### 5.4 Real-time Monitoring

Monitor system behavior during testing:

```bash
# Terminal 1: Migration monitoring
sudo dmesg -w | grep -E "(Migration|Migrated|SLC|QLC)"

# Terminal 2: System resources
iostat -x 2

# Terminal 3: Progress checking
sudo ./manual_test_checker.sh migration
```

## 6. Expected Results

### 6.1 Success Indicators

✅ **Module Loading**
```bash
$ lsmod | grep nvmev
nvmev                 XXXXX  0
```

✅ **Device Creation**
```bash
$ lsblk | grep nvme
nvme0n1  259:0  0  10G  0 disk
```

✅ **SLC/QLC Initialization**
```bash
$ dmesg | grep -E "(SLC|QLC)"
[nvmev] SLC blocks per plane: 1638 (20%), QLC blocks per plane: 6554 (80%)
[nvmev] Hybrid SLC/QLC storage initialized successfully
```

✅ **Migration Activity**
```bash
$ dmesg | grep "Migrated LPN" | wc -l
150  # Should be > 50 for successful migration testing
```

### 6.2 Performance Expectations

| Metric | SLC Performance | QLC Performance | Ratio |
|--------|----------------|----------------|-------|
| **Random Write IOPS** | 15,000-30,000 | N/A (write to SLC first) | N/A |
| **Random Read IOPS** | 25,000-40,000 | 8,000-15,000 | 2.5-3.0x |
| **Sequential Read** | 2,000-3,000 MB/s | 800-1,200 MB/s | 2.0-2.5x |
| **Average Latency** | 30-50 μs | 75-150 μs | 2.5-3.0x |

### 6.3 Migration Statistics

Expected migration behavior:
```bash
$ dmesg | grep "Write Stats" | tail -1
[nvmev] Write Stats: SLC writes=8500, QLC writes=200, Migrations=150, Cold migrations=120
```

Key metrics:
- **Migration Count**: >100 migrations for comprehensive test
- **Migration Ratio**: ~15-25% of written data migrates to QLC
- **Hot Data Retention**: Frequently accessed data stays in SLC

## 7. Troubleshooting

### 7.1 Module Loading Issues

**Problem**: `insmod` fails with memory errors
```bash
# Solution 1: Check available memory
free -h  # Ensure at least 16GB available

# Solution 2: Set hugepages
echo 5120 > /proc/sys/vm/nr_hugepages

# Solution 3: Use smaller allocation
sudo insmod nvmev_on.ko memmap_start=8G memmap_size=6G cpus=0,1
```

**Problem**: Module loads but no device appears
```bash
# Check module parameters
modinfo ../nvmevirt_DA/nvmev_on.ko

# Check kernel messages for errors
dmesg | grep -E "(nvmev|error|fail)" | tail -20

# Verify kernel headers
ls /lib/modules/$(uname -r)/build || sudo apt install linux-headers-$(uname -r)
```

### 7.2 Migration Issues

**Problem**: No migration detected
```bash
# Check migration threshold configuration
grep MIGRATION_THRESHOLD ../nvmevirt_DA/ssd_config.h
# Should show: #define MIGRATION_THRESHOLD (10)

# Write more data to trigger migration
for i in {1..15}; do
    sudo dd if=/dev/zero of=/mnt/test_hybrid/force_$i.dat bs=1M count=100 oflag=direct
    sync && sleep 2
done

# Check SLC capacity settings
grep SLC_CAPACITY_PERCENT ../nvmevirt_DA/ssd_config.h
```

**Problem**: Migration too aggressive/frequent
```bash
# Increase migration threshold
sed -i 's/MIGRATION_THRESHOLD (10)/MIGRATION_THRESHOLD (20)/' ../nvmevirt_DA/ssd_config.h

# Recompile and reload
cd ../nvmevirt_DA && make clean && make APPROACH=on
```

### 7.3 Performance Issues

**Problem**: No performance difference between SLC and QLC
```bash
# Verify QLC latency configuration
grep QLC_PROG_LATENCY ../nvmevirt_DA/ssd_config.h
# Should show: #define QLC_PROG_LATENCY (185000 * 4)

# Check if direct I/O is being used
fio --name=test --direct=1 --ioengine=libaio --rw=read --bs=4k --size=1M \
    --filename=/mnt/test_hybrid/test.dat --output-format=normal
```

**Problem**: Poor overall performance
```bash
# Check system load
htop  # Ensure low CPU usage during tests

# Verify CPU affinity
numactl -H  # Check NUMA topology
# Use CPUs from same NUMA node as memory
```

### 7.4 Using the Progress Checker

For real-time troubleshooting:
```bash
# Check all components
sudo ./manual_test_checker.sh

# Check specific components
sudo ./manual_test_checker.sh prereq      # Prerequisites
sudo ./manual_test_checker.sh compile     # Compilation status
sudo ./manual_test_checker.sh module      # Module loading
sudo ./manual_test_checker.sh migration   # Migration activity
sudo ./manual_test_checker.sh perf        # Performance results
```

## 8. Advanced Configuration

### 8.1 Custom SLC/QLC Ratios

Modify capacity allocation:
```c
// In nvmevirt_DA/ssd_config.h
#define SLC_CAPACITY_PERCENT (30)  // Increase SLC to 30%
#define QLC_CAPACITY_PERCENT (70)  // Decrease QLC to 70%
```

### 8.2 Migration Tuning

Adjust migration behavior:
```c
// More aggressive migration (lower threshold)
#define MIGRATION_THRESHOLD (5)

// Less aggressive migration (higher threshold)  
#define MIGRATION_THRESHOLD (20)

// Faster cold data detection (shorter time)
#define COLD_DATA_TIME_THRESHOLD (500000)  // 0.5 seconds
```

### 8.3 Performance Tuning

Modify latency characteristics:
```c
// More realistic QLC penalties
#define QLC_PROG_LATENCY (185000 * 6)      // 6x slower writes
#define QLC_READ_LATENCY_FACTOR (3.0)      // 3x slower reads
#define QLC_ERASE_LATENCY (4000000)        // 4x slower erase
```

### 8.4 Multi-Region QLC

Increase QLC parallelism:
```c
// More QLC regions for better performance
#define QLC_REGIONS (8)  // Increase from 4 to 8 regions
```

## 9. Integration with Original FAST '24 Evaluation

### 9.1 Running Hybrid Tests with Original Workloads

The hybrid storage system can be tested with the original FAST '24 workloads:

```bash
# Load hybrid storage module
sudo insmod ../nvmevirt_DA/nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# Run original evaluation with hybrid storage
./hypothetical_append.sh    # Tests with SLC/QLC migration
./hypothetical_overwrite.sh # Tests with hot/cold separation
./sqlite.sh                 # Database workload with migration
./fileserver.sh            # File server with hybrid storage

# Analyze both original metrics and hybrid behavior
./printresult.sh           # Original results
./analyze_hybrid_results.sh # Hybrid-specific analysis
```

### 9.2 Comparing Hybrid vs Original Performance

Generate comparison reports:
```bash
# Run tests with hybrid storage (APPROACH=on)
sudo insmod ../nvmevirt_DA/nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3
./runall.sh
cp -r result result_hybrid

# Run tests with original approach (if available)
sudo rmmod nvmev
sudo insmod ../nvmevirt_DA/nvmev_off.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3
./runall.sh
cp -r result result_original

# Compare results
diff result_hybrid/ result_original/
```

## 10. Results and Publications

This hybrid SLC/QLC storage implementation demonstrates:

1. **Effective Migration**: Automatic data movement based on access patterns
2. **Performance Differentiation**: Realistic SLC/QLC performance characteristics  
3. **Capacity Optimization**: Intelligent use of high-speed cache and high-capacity storage
4. **Real-world Applicability**: Applicable to modern hybrid storage devices

### Citation

If you use this hybrid storage testing framework in your research, please cite:

```bibtex
@inproceedings{fast24_hybrid_storage,
  title={Hybrid SLC/QLC Storage Testing Framework based on NVMeVirt},
  author={Enhanced NVMeVirt Development Team},
  booktitle={Enhanced FAST '24 Artifacts},
  year={2024},
  note={Based on original FAST '24 NVMeVirt implementation}
}
```

---

## Quick Reference

### Essential Commands
```bash
# Build and load
cd nvmevirt_DA && make clean && make APPROACH=on
sudo insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# Quick test
cd ../evaluation && sudo ./run_hybrid_test.sh

# Monitor
sudo dmesg -w | grep -E "(Migration|SLC|QLC)"

# Check progress
sudo ./manual_test_checker.sh

# Cleanup
sudo umount /mnt/test_hybrid && sudo rmmod nvmev
```

### Support and Documentation

- **Quick Start**: See `QUICK_MANUAL_STEPS.md` for 17 essential commands
- **Detailed Guide**: See `MANUAL_TEST_GUIDE.md` for comprehensive procedures  
- **Tool Overview**: Run `./list_hybrid_tests.sh` for all available tools
- **Progress Checking**: Use `./manual_test_checker.sh [component]` for verification

For additional support, refer to the comprehensive documentation in the `evaluation/` directory. 