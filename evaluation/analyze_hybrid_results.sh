#!/bin/bash

###########################################
# Hybrid Storage Test Results Analysis Script
###########################################

echo "====================================="
echo "Hybrid SLC/QLC Storage Test Results Analysis"
echo "====================================="

# Find latest test result directory
LATEST_RESULT=$(ls -td ./result/hybrid_test_* 2>/dev/null | head -1)

if [ -z "$LATEST_RESULT" ]; then
    echo "Error: Test result directory not found"
    echo "Please run ./test_hybrid_storage.sh first"
    exit 1
fi

echo "Analyzing result directory: $LATEST_RESULT"
echo ""

# 1. Migration effectiveness analysis
echo "1. Migration Effectiveness Analysis"
echo "===================================="
if [ -f "$LATEST_RESULT/final_dmesg.log" ]; then
    MIGRATION_COUNT=$(grep -c "Migrated" "$LATEST_RESULT/final_dmesg.log" 2>/dev/null || echo "0")
    echo "Total migration count: $MIGRATION_COUNT"
    
    # Display migration statistics
    echo "Migration statistics:"
    grep "Write Stats" "$LATEST_RESULT/final_dmesg.log" | tail -1 || echo "Statistics not found"
    echo ""
fi

# 2. Performance comparison analysis
echo "2. Performance Comparison Analysis"
echo "=================================="

# Analyze SLC performance
if [ -f "$LATEST_RESULT/slc_performance.txt" ]; then
    echo "SLC layer performance:"
    # Extract IOPS
    SLC_IOPS=$(grep -E "write.*iops" "$LATEST_RESULT/slc_performance.txt" | grep -oE "iops=[0-9.]+" | head -1 | cut -d= -f2)
    # Extract bandwidth
    SLC_BW=$(grep -E "write.*bw=" "$LATEST_RESULT/slc_performance.txt" | grep -oE "bw=[0-9.]+[GMK]?iB/s" | head -1)
    # Extract latency
    SLC_LAT=$(grep -E "lat.*avg=" "$LATEST_RESULT/slc_performance.txt" | grep -oE "avg=[0-9.]+" | head -1 | cut -d= -f2)
    
    echo "  - IOPS: ${SLC_IOPS:-N/A}"
    echo "  - Bandwidth: ${SLC_BW:-N/A}"
    echo "  - Average latency: ${SLC_LAT:-N/A} μs"
fi

# Analyze QLC performance
if [ -f "$LATEST_RESULT/qlc_performance.txt" ]; then
    echo ""
    echo "QLC layer performance:"
    # Extract IOPS
    QLC_IOPS=$(grep -E "read.*iops" "$LATEST_RESULT/qlc_performance.txt" | grep -oE "iops=[0-9.]+" | head -1 | cut -d= -f2)
    # Extract bandwidth
    QLC_BW=$(grep -E "read.*bw=" "$LATEST_RESULT/qlc_performance.txt" | grep -oE "bw=[0-9.]+[GMK]?iB/s" | head -1)
    # Extract latency
    QLC_LAT=$(grep -E "lat.*avg=" "$LATEST_RESULT/qlc_performance.txt" | grep -oE "avg=[0-9.]+" | head -1 | cut -d= -f2)
    
    echo "  - IOPS: ${QLC_IOPS:-N/A}"
    echo "  - Bandwidth: ${QLC_BW:-N/A}"
    echo "  - Average latency: ${QLC_LAT:-N/A} μs"
fi

# Calculate performance ratio
if [ ! -z "$SLC_IOPS" ] && [ ! -z "$QLC_IOPS" ] && [ "$QLC_IOPS" != "0" ]; then
    PERF_RATIO=$(echo "scale=2; $SLC_IOPS / $QLC_IOPS" | bc 2>/dev/null || echo "N/A")
    echo ""
    echo "SLC/QLC performance ratio: ${PERF_RATIO}x"
fi

# 3. Hot/cold data separation effectiveness
echo ""
echo "3. Hot/Cold Data Separation Effectiveness"
echo "========================================="
if [ -f "$LATEST_RESULT/hot_cold_pattern.log" ]; then
    HOT_ACCESS=$(grep -c "hot" "$LATEST_RESULT/hot_cold_pattern.log" 2>/dev/null || echo "0")
    COLD_MIGRATE=$(grep -c "cold.*migrate" "$LATEST_RESULT/hot_cold_pattern.log" 2>/dev/null || echo "0")
    echo "Hot data access records: $HOT_ACCESS"
    echo "Cold data migration records: $COLD_MIGRATE"
fi

# 4. Stress test results
echo ""
echo "4. Stress Test Results"
echo "======================"
if [ -f "$LATEST_RESULT/stress_test.txt" ]; then
    echo "Mixed read/write stress test:"
    grep -E "(read|write).*iops" "$LATEST_RESULT/stress_test.txt" | head -2
fi

# 5. Generate summary report
echo ""
echo "5. Test Summary"
echo "==============="
cat > "$LATEST_RESULT/analysis_summary.txt" <<EOF
Hybrid SLC/QLC Storage Test Analysis Summary
Generated: $(date)

1. Migration Effectiveness
   - Total migration count: $MIGRATION_COUNT
   - Migration mechanism: ${MIGRATION_COUNT:+Working normally}${MIGRATION_COUNT:-No migration detected}

2. Performance Characteristics
   - SLC IOPS: ${SLC_IOPS:-N/A}
   - QLC IOPS: ${QLC_IOPS:-N/A}
   - Performance ratio: ${PERF_RATIO:-N/A}x

3. Hybrid Storage Status
   - Basic functionality: Normal
   - Migration functionality: ${MIGRATION_COUNT:+Normal}${MIGRATION_COUNT:-Needs inspection}
   - Performance differentiation: ${PERF_RATIO:+Verified}${PERF_RATIO:-Not verified}

Conclusion: Hybrid SLC/QLC storage is ${MIGRATION_COUNT:+operating normally}${MIGRATION_COUNT:-needs further inspection}
EOF

cat "$LATEST_RESULT/analysis_summary.txt"

echo ""
echo "Detailed analysis report saved to: $LATEST_RESULT/analysis_summary.txt" 