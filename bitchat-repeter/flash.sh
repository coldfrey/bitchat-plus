#!/bin/bash
# Flash script for BitChat Repeater devices
# This script builds and flashes firmware to two ESP32 devices

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Device ports
DEVICE1="/dev/cu.usbserial-5"
DEVICE2="/dev/cu.usbserial-0001"

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  BitChat Repeater Flash Tool${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Function to check if a device is connected
check_device() {
    local port=$1
    if [ -e "$port" ]; then
        echo -e "${GREEN}✓${NC} Found device at $port"
        return 0
    else
        echo -e "${RED}✗${NC} Device not found at $port"
        return 1
    fi
}

# Function to kill processes using a port
kill_port_process() {
    local port=$1
    local pids=$(lsof -t "$port" 2>/dev/null)
    
    if [ ! -z "$pids" ]; then
        echo -e "${YELLOW}Killing process(es) using $port: $pids${NC}"
        kill -9 $pids 2>/dev/null
        sleep 1
    fi
}

# Function to flash a single device
flash_device() {
    local port=$1
    local device_name=$2
    
    echo ""
    echo -e "${BLUE}Flashing $device_name at $port...${NC}"
    
    # Kill any processes using this port
    kill_port_process "$port"
    
    # Flash the device
    if pio run --target upload --upload-port "$port"; then
        echo -e "${GREEN}✓ Successfully flashed $device_name${NC}"
        return 0
    else
        echo -e "${RED}✗ Failed to flash $device_name${NC}"
        return 1
    fi
}

# Function to build firmware once
build_firmware() {
    echo -e "${BLUE}Building firmware...${NC}"
    
    if pio run; then
        echo -e "${GREEN}✓ Firmware build successful${NC}"
        return 0
    else
        echo -e "${RED}✗ Firmware build failed${NC}"
        return 1
    fi
}

# Main script
main() {
    # Check PlatformIO installation
    if ! command -v pio &> /dev/null; then
        echo -e "${RED}Error: PlatformIO CLI not found. Please install it first.${NC}"
        echo "Install with: pip install platformio"
        exit 1
    fi
    
    # Check for devices
    echo "Checking for connected devices..."
    device1_found=false
    device2_found=false
    
    if check_device "$DEVICE1"; then
        device1_found=true
    fi
    
    if check_device "$DEVICE2"; then
        device2_found=true
    fi
    
    if [ "$device1_found" = false ] && [ "$device2_found" = false ]; then
        echo -e "${RED}No devices found! Please connect your ESP32 devices.${NC}"
        exit 1
    fi
    
    # Build firmware once
    echo ""
    if ! build_firmware; then
        echo -e "${RED}Build failed. Please fix compilation errors.${NC}"
        exit 1
    fi
    
    # Flash devices
    flash_count=0
    fail_count=0
    
    if [ "$device1_found" = true ]; then
        if flash_device "$DEVICE1" "Device 1"; then
            ((flash_count++))
        else
            ((fail_count++))
        fi
    fi
    
    if [ "$device2_found" = true ]; then
        if flash_device "$DEVICE2" "Device 2"; then
            ((flash_count++))
        else
            ((fail_count++))
        fi
    fi
    
    # Summary
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Flash Summary${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo -e "Devices flashed successfully: ${GREEN}$flash_count${NC}"
    if [ $fail_count -gt 0 ]; then
        echo -e "Devices failed: ${RED}$fail_count${NC}"
    fi
    
    # Offer to open serial monitors
    if [ $flash_count -gt 0 ]; then
        echo ""
        echo -e "${YELLOW}Would you like to open serial monitors? (y/n)${NC}"
        read -r response
        
        if [[ "$response" =~ ^[Yy]$ ]]; then
            if [ "$device1_found" = true ] && [ -e "$DEVICE1" ]; then
                echo "Opening monitor for Device 1..."
                osascript -e "tell app \"Terminal\" to do script \"cd '$SCRIPT_DIR' && pio device monitor --port $DEVICE1 --echo\""
            fi
            
            if [ "$device2_found" = true ] && [ -e "$DEVICE2" ]; then
                echo "Opening monitor for Device 2..."
                osascript -e "tell app \"Terminal\" to do script \"cd '$SCRIPT_DIR' && pio device monitor --port $DEVICE2 --echo\""
            fi
        fi
    fi
}

# Run main function
main