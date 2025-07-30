# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BitChat is a decentralized peer-to-peer messaging app that operates over Bluetooth mesh networks. It's a universal Swift app supporting both iOS (16.0+) and macOS (13.0+) with no internet dependency. The project now includes **Gateway** functionality for extended range via LoRa repeaters.

### Key Architecture Components

- **SwiftUI App**: Universal app with platform-specific adaptations
- **Bluetooth Mesh Networking**: Dual-mode (central/peripheral) BLE mesh using `BluetoothMeshService`
- **Binary Protocol**: Custom efficient protocol in `BitchatProtocol` optimized for BLE constraints
- **Noise Protocol**: End-to-end encryption using Noise Protocol Framework for private messages
- **Store & Forward**: Message caching and delivery for offline peers
- **Gateway System**: LoRa-based range extension with ESP32 repeaters and iOS `GatewayKit`
- **Hardware Component**: ESP32-based gateways (`bitchat-repeter/`) using PlatformIO

### Core Services Architecture

- `BluetoothMeshService`: Core mesh networking, peer discovery, connection management
- `NoiseEncryptionService`: E2E encryption integration with Noise Protocol
- `ChatViewModel`: Main app state management and UI coordination
- `SecureIdentityStateManager`: Identity and key management with Keychain integration
- `MessageRetryService`: Reliable message delivery with exponential backoff
- `DeliveryTracker`: Message acknowledgment and delivery tracking
- `GatewayKit`: Swift package for LoRa gateway communication (BLE ↔ LoRa bridge)
- `TransportMux`: Transport multiplexer combining direct BLE and gateway transports

### Protocol Layer

The app uses a custom binary protocol (`BitchatProtocol`) with:
- 1-byte message type identifiers
- TTL-based routing (max 7 hops)
- Message fragmentation for large payloads
- Privacy features (padding, timing obfuscation)
- Integration points for Noise encryption

## Development Commands

### Building and Running

**iOS/macOS Universal App:**
```bash
# Generate Xcode project (recommended)
xcodegen generate
open bitchat.xcodeproj

# Using Swift Package Manager
open Package.swift

# Quick macOS-only build and run
just run

# Clean build artifacts and restore files
just clean
```

**Hardware Gateways (ESP32):**
```bash
cd bitchat-repeter
# Build and upload Gateway firmware to ESP32
pio run --target upload
# Monitor serial output for debugging
pio device monitor
# Flash both bootloader and app
./flash_both.sh
```

### Testing

**Swift Tests:**
```bash
# iOS tests
xcodebuild test -project bitchat.xcodeproj -scheme "bitchat (iOS)" -destination "platform=iOS Simulator,name=iPhone 15"

# macOS tests  
xcodebuild test -project bitchat.xcodeproj -scheme "bitchat (macOS)"
```

**Hardware Tests:**
```bash
cd bitchat-repeter
pio test
# Test Gateway functionality
pio run --target test
```

**GatewayKit Tests:**
```bash
# Swift package tests
swift test --package-path bitchat/GatewayKit
# Via Xcode
xcodebuild test -project bitchat.xcodeproj -scheme "GatewayKit-Package"
```

### Build System Details

- **XcodeGen**: Primary build system using `project.yml` configuration
- **Just**: Convenience commands for macOS development (`Justfile`)
- **PlatformIO**: ESP32 firmware development and deployment
- Multi-target project: iOS app, macOS app, Share Extension, Unit Tests

## Key Development Patterns

### Platform-Specific Code
Uses compiler directives for platform differences:
```swift
#if os(iOS)
// iOS-specific code
#elseif os(macOS)  
// macOS-specific code
#endif
```

### Mesh Networking
- Each device acts as both central and peripheral
- TTL-based message routing with loop prevention
- Bloom filter-based message deduplication
- Lazy Noise handshakes for private messages

### Identity Management
- No persistent user accounts or phone numbers
- Ephemeral identities with optional nicknames
- Secure key storage in system Keychain
- Emergency wipe capability (triple-tap logo)

### Message Flow
1. User input → `ChatViewModel`
2. Protocol encoding → `BitchatProtocol`
3. Optional encryption → `NoiseEncryptionService`
4. Mesh routing → `BluetoothMeshService`
5. BLE transmission → Core Bluetooth

## Testing Architecture

- **Unit Tests**: Protocol encoding/decoding, crypto operations, utilities
- **Integration Tests**: Service interactions, mesh routing scenarios  
- **End-to-End Tests**: Full message flow from UI to protocol layer
- **Mock Objects**: `MockBluetoothMeshService`, `MockNoiseSession` for testing

## Gateway Integration

### Hardware Gateways
The `bitchat-repeter/` contains ESP32 firmware for LoRa gateways:
- **Hardware**: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
- **Function**: BLE ↔ LoRa bridge extending BitChat range
- **Protocol**: Custom Bridge Header with TTL routing and de-duplication
- **Features**: Rolling EID privacy, duty cycle compliance, live configuration
- **Architecture**: Multi-task RTOS design (BLE RX/TX, LoRa RX/TX, Router, EID rotation)

### iOS Gateway Support  
The `GatewayKit` Swift package provides:
- **GatewayTransport**: CoreBluetooth client for gateway connection
- **Bridge Protocol**: Binary header format matching ESP32 implementation
- **Fragmentation**: Automatic message fragmentation for BLE MTU limits
- **Statistics**: Real-time gateway performance monitoring
- **Integration**: Transport multiplexer combining direct + gateway modes

## Security Considerations

- Uses Noise Protocol XX pattern for authenticated encryption
- No metadata leaked in protocol headers
- Cover traffic and timing obfuscation for privacy
- Emergency wipe destroys all local data
- Code is public domain - security through transparency