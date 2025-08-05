#!/bin/bash

###########################################
# Manual Test Progress Checker
# This script helps verify each step of manual testing
###########################################

echo "=============================================="
echo "Hybrid SLC/QLC Manual Test Progress Checker"
echo "=============================================="

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print status
print_status() {
    local status=$1
    local message=$2
    case $status in
        "PASS")
            echo -e "${GREEN}[✓] $message${NC}"
            ;;
        "FAIL")
            echo -e "${RED}[✗] $message${NC}"
            ;;
        "WARN")
            echo -e "${YELLOW}[!] $message${NC}"
            ;;
        "INFO")
            echo -e "${BLUE}[i] $message${NC}"
            ;;
    esac
}

# Function to check prerequisites
check_prerequisites() {
    echo -e "\n${BLUE}=== Prerequisites Check ===${NC}"
    
    # Check kernel version
    KERNEL_VERSION=$(uname -r | cut -d. -f1-2)
    if [ "$(echo "$KERNEL_VERSION >= 5.0" | bc)" -eq 1 ]; then
        print_status "PASS" "Kernel version: $(uname -r)"
    else
        print_status "FAIL" "Kernel version too old: $(uname -r) (need >= 5.0)"
    fi
    
    # Check memory
    MEMORY_GB=$(free -g | grep "Mem:" | awk '{print $2}')
    if [ $MEMORY_GB -ge 16 ]; then
        print_status "PASS" "System memory: ${MEMORY_GB}GB"
    else
        print_status "WARN" "System memory: ${MEMORY_GB}GB (recommended: >= 16GB)"
    fi
    
    # Check if running as root
    if [ "$EUID" -eq 0 ]; then
        print_status "PASS" "Running with root privileges"
    else
        print_status "FAIL" "Not running as root (use sudo)"
    fi
    
    # Check required tools
    for tool in fio bc iostat; do
        if command -v $tool &> /dev/null; then
            print_status "PASS" "Tool available: $tool"
        else
            print_status "FAIL" "Missing tool: $tool"
        fi
    done
}

# Function to check compilation
check_compilation() {
    echo -e "\n${BLUE}=== Compilation Check ===${NC}"
    
    # Check if in correct directory
    if [ -f "../nvmevirt_DA/Makefile" ]; then
        print_status "PASS" "NVMeVirt directory found"
    else
        print_status "FAIL" "NVMeVirt directory not found (run from evaluation/)"
        return 1
    fi
    
    # Check if module is compiled
    if [ -f "../nvmevirt_DA/nvmev_on.ko" ]; then
        print_status "PASS" "Module compiled: nvmev_on.ko"
        
        # Check module size (should be reasonable)
        MODULE_SIZE=$(stat -c%s "../nvmevirt_DA/nvmev_on.ko")
        if [ $MODULE_SIZE -gt 100000 ]; then
            print_status "PASS" "Module size: $(($MODULE_SIZE/1024))KB"
        else
            print_status "WARN" "Module size seems small: $(($MODULE_SIZE/1024))KB"
        fi
    else
        print_status "FAIL" "Module not compiled (run: make APPROACH=on)"
    fi
    
    # Check hybrid configurations
    if grep -q "SLC_CAPACITY_PERCENT" ../nvmevirt_DA/ssd_config.h; then
        print_status "PASS" "Hybrid configuration found in ssd_config.h"
    else
        print_status "FAIL" "Hybrid configuration missing"
    fi
}

# Function to check module status
check_module_status() {
    echo -e "\n${BLUE}=== Module Status Check ===${NC}"
    
    # Check if module is loaded
    if lsmod | grep -q nvmev; then
        print_status "PASS" "NVMeVirt module is loaded"
        
        # Show module info
        MODULE_INFO=$(lsmod | grep nvmev)
        print_status "INFO" "Module info: $MODULE_INFO"
    else
        print_status "FAIL" "NVMeVirt module not loaded"
        return 1
    fi
    
    # Check for NVMe device
    if lsblk | grep -q nvme; then
        NVME_DEVICE=$(lsblk | grep nvme | head -1 | awk '{print $1}')
        print_status "PASS" "NVMe device created: $NVME_DEVICE"
    else
        print_status "FAIL" "No NVMe device found"
    fi
    
    # Check recent kernel messages
    RECENT_NVMEV_MSGS=$(dmesg | grep nvmev | tail -5 | wc -l)
    if [ $RECENT_NVMEV_MSGS -gt 0 ]; then
        print_status "PASS" "Recent nvmev messages found in dmesg"
    else
        print_status "WARN" "No recent nvmev messages in dmesg"
    fi
}

# Function to check filesystem status
check_filesystem_status() {
    echo -e "\n${BLUE}=== Filesystem Status Check ===${NC}"
    
    # Check if device is mounted
    if mount | grep -q nvme; then
        MOUNT_INFO=$(mount | grep nvme)
        print_status "PASS" "NVMe device mounted: $MOUNT_INFO"
        
        # Check mount point
        if [ -d "/mnt/test_hybrid" ]; then
            print_status "PASS" "Mount point exists: /mnt/test_hybrid"
        else
            print_status "WARN" "Standard mount point not found"
        fi
    else
        print_status "FAIL" "NVMe device not mounted"
    fi
}

# Function to check migration status
check_migration_status() {
    echo -e "\n${BLUE}=== Migration Status Check ===${NC}"
    
    # Count migration messages
    MIGRATION_COUNT=$(dmesg | grep -c "Migrated LPN" 2>/dev/null || echo "0")
    if [ $MIGRATION_COUNT -gt 0 ]; then
        print_status "PASS" "Migrations detected: $MIGRATION_COUNT"
        
        # Show latest migration statistics
        WRITE_STATS=$(dmesg | grep "Write Stats" | tail -1)
        if [ ! -z "$WRITE_STATS" ]; then
            print_status "INFO" "Latest stats: $WRITE_STATS"
        fi
    else
        print_status "WARN" "No migrations detected yet"
    fi
    
    # Check SLC/QLC messages
    SLC_MSGS=$(dmesg | grep -c "SLC" 2>/dev/null || echo "0")
    QLC_MSGS=$(dmesg | grep -c "QLC" 2>/dev/null || echo "0")
    
    if [ $SLC_MSGS -gt 0 ]; then
        print_status "PASS" "SLC messages found: $SLC_MSGS"
    else
        print_status "WARN" "No SLC messages found"
    fi
    
    if [ $QLC_MSGS -gt 0 ]; then
        print_status "PASS" "QLC messages found: $QLC_MSGS"
    else
        print_status "WARN" "No QLC messages found"
    fi
}

# Function to check performance test results
check_performance_results() {
    echo -e "\n${BLUE}=== Performance Results Check ===${NC}"
    
    # Check for performance files
    if [ -f "/tmp/slc_performance.txt" ]; then
        print_status "PASS" "SLC performance file found"
        
        # Extract IOPS
        SLC_IOPS=$(grep "iops" /tmp/slc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2 2>/dev/null)
        if [ ! -z "$SLC_IOPS" ]; then
            print_status "INFO" "SLC IOPS: $SLC_IOPS"
        fi
    else
        print_status "WARN" "SLC performance file not found"
    fi
    
    if [ -f "/tmp/qlc_performance.txt" ]; then
        print_status "PASS" "QLC performance file found"
        
        # Extract IOPS
        QLC_IOPS=$(grep "iops" /tmp/qlc_performance.txt | head -1 | grep -oE "iops=[0-9.]+" | cut -d= -f2 2>/dev/null)
        if [ ! -z "$QLC_IOPS" ]; then
            print_status "INFO" "QLC IOPS: $QLC_IOPS"
        fi
    else
        print_status "WARN" "QLC performance file not found"
    fi
    
    # Calculate ratio if both available
    if [ ! -z "$SLC_IOPS" ] && [ ! -z "$QLC_IOPS" ] && [ "$QLC_IOPS" != "0" ]; then
        RATIO=$(echo "scale=2; $SLC_IOPS / $QLC_IOPS" | bc 2>/dev/null)
        if [ ! -z "$RATIO" ]; then
            print_status "INFO" "SLC/QLC Performance Ratio: ${RATIO}x"
            
            # Check if ratio is reasonable
            if [ "$(echo "$RATIO >= 2.0" | bc)" -eq 1 ]; then
                print_status "PASS" "Performance ratio is reasonable (>= 2.0x)"
            else
                print_status "WARN" "Performance ratio may be low (< 2.0x)"
            fi
        fi
    fi
}

# Function to check test files
check_test_files() {
    echo -e "\n${BLUE}=== Test Files Check ===${NC}"
    
    # Check for basic test files
    if [ -f "/mnt/test_hybrid/basic_tests/test1.dat" ]; then
        print_status "PASS" "Basic test file exists"
    else
        print_status "WARN" "Basic test file not found"
    fi
    
    # Check for migration test files
    MIGRATION_FILES=$(ls /mnt/test_hybrid/migration_test_*.dat 2>/dev/null | wc -l)
    if [ $MIGRATION_FILES -gt 0 ]; then
        print_status "PASS" "Migration test files found: $MIGRATION_FILES"
    else
        print_status "WARN" "Migration test files not found"
    fi
    
    # Check for hot/cold data files
    if [ -f "/mnt/test_hybrid/hot_data.txt" ]; then
        print_status "PASS" "Hot data file exists"
    else
        print_status "WARN" "Hot data file not found"
    fi
    
    if [ -f "/mnt/test_hybrid/cold_data.dat" ]; then
        print_status "PASS" "Cold data file exists"
    else
        print_status "WARN" "Cold data file not found"
    fi
}

# Function to show system status
show_system_status() {
    echo -e "\n${BLUE}=== Current System Status ===${NC}"
    
    # Show memory usage
    print_status "INFO" "Memory: $(free -h | grep Mem: | awk '{print "Used " $3 " / " $2}')"
    
    # Show CPU load
    LOAD_AVG=$(uptime | awk '{print $NF}')
    print_status "INFO" "Load average: $LOAD_AVG"
    
    # Show disk space
    if [ -d "/mnt/test_hybrid" ]; then
        DISK_USAGE=$(df -h /mnt/test_hybrid | tail -1 | awk '{print "Used " $3 " / " $2 " (" $5 ")"}')
        print_status "INFO" "Test filesystem: $DISK_USAGE"
    fi
    
    # Show recent dmesg errors
    ERROR_COUNT=$(dmesg | grep -i error | tail -10 | wc -l)
    if [ $ERROR_COUNT -gt 0 ]; then
        print_status "WARN" "Recent errors in dmesg: $ERROR_COUNT"
    else
        print_status "PASS" "No recent errors in dmesg"
    fi
}

# Function to provide recommendations
provide_recommendations() {
    echo -e "\n${BLUE}=== Recommendations ===${NC}"
    
    # Check module status
    if ! lsmod | grep -q nvmev; then
        print_status "INFO" "Load module: sudo insmod ../nvmevirt_DA/nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3"
    fi
    
    # Check filesystem
    if ! mount | grep -q nvme; then
        print_status "INFO" "Mount filesystem: sudo mount /dev/nvme0n1 /mnt/test_hybrid"
    fi
    
    # Check migration
    MIGRATION_COUNT=$(dmesg | grep -c "Migrated LPN" 2>/dev/null || echo "0")
    if [ $MIGRATION_COUNT -eq 0 ]; then
        print_status "INFO" "To trigger migration: Write large files (>1GB total) to /mnt/test_hybrid"
    fi
    
    # Check performance tests
    if [ ! -f "/tmp/slc_performance.txt" ]; then
        print_status "INFO" "Run SLC performance test: fio --name=slc_test --rw=randwrite --bs=4k --direct=1 --size=50M --runtime=30 --filename=/mnt/test_hybrid/slc_test.dat"
    fi
}

# Main execution
main() {
    local check_type="${1:-all}"
    
    case $check_type in
        "prereq"|"prerequisites")
            check_prerequisites
            ;;
        "compile"|"compilation")
            check_compilation
            ;;
        "module")
            check_module_status
            ;;
        "fs"|"filesystem")
            check_filesystem_status
            ;;
        "migration")
            check_migration_status
            ;;
        "perf"|"performance")
            check_performance_results
            ;;
        "files")
            check_test_files
            ;;
        "status")
            show_system_status
            ;;
        "help")
            echo "Usage: $0 [check_type]"
            echo "Check types:"
            echo "  prereq      - Check prerequisites"
            echo "  compile     - Check compilation status"
            echo "  module      - Check module loading status"
            echo "  fs          - Check filesystem status"
            echo "  migration   - Check migration status"
            echo "  perf        - Check performance test results"
            echo "  files       - Check test files"
            echo "  status      - Show system status"
            echo "  all         - Run all checks (default)"
            ;;
        "all"|*)
            check_prerequisites
            check_compilation
            check_module_status
            check_filesystem_status
            check_migration_status
            check_performance_results
            check_test_files
            show_system_status
            provide_recommendations
            ;;
    esac
}

# Run main function with arguments
main "$@"

echo -e "\n${BLUE}=============================================="
echo "Manual Test Checker Complete"
echo -e "==============================================${NC}" 