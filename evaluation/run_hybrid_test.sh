#!/bin/bash

###########################################
# Hybrid Storage Test Quick Start Script
###########################################

echo "====================================="
echo "Hybrid SLC/QLC Storage Test Launcher"
echo "====================================="

# Check if running with root privileges
if [ "$EUID" -ne 0 ]; then 
    echo "Error: This script requires root privileges"
    echo "Please use: sudo $0"
    exit 1
fi

# Check necessary tools
echo "Checking necessary tools..."
for tool in fio mkfs.ext4; do
    if ! command -v $tool &> /dev/null; then
        echo "Error: Missing tool $tool"
        echo "Please install: sudo apt-get install $tool"
        exit 1
    fi
done

# Run complete hybrid storage test
echo "Starting hybrid storage test..."
./test_hybrid_storage.sh

# Ask if standard test suite should be run
read -p "Run standard test suite? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Running standard test suite..."
    ./hypothetical_append.sh
    ./hypothetical_overwrite.sh
    ./sqlite.sh
    ./fileserver.sh
    ./fileserver_small.sh
    ./printresult.sh
fi

echo "All tests completed!" 