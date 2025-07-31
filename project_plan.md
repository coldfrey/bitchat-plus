# BitChat Repeater Firmware Development Plan
# Zero App Changes - Pure Mesh Extension

## Overview
This document contains a series of prompts for implementing the BitChat repeater firmware. The repeater will mimic a regular BitChat device, appearing as a normal peer to iOS apps while internally bridging messages over LoRa mesh networks to extend range dramatically.

## Key Design Principles
1. **Complete Protocol Compatibility** - Uses existing BitChat BLE protocol for iOS connections
2. **Transparent Operation** - iOS apps see repeaters as regular peers
3. **Dual Transport Architecture** - BLE for iOS devices, LoRa mesh for repeater networks
4. **Automatic Mesh Extension** - No configuration needed, self-organizing network
5. **Smart Routing Control** - Prevents loops while optimizing delivery paths

---

## Phase 1: Foundation Setup

### Prompt 1.1: Project Structure
**Status: [x] Completed**

Create the basic project structure for the bitchat-repeater firmware:
1. Set up PlatformIO project for Heltec WiFi LoRa 32 V3 (ESP32-S3)
2. Create modular file structure:
   - src/main.cpp (minimal, just setup/loop)
   - src/ble_mesh.cpp/h (BitChat BLE protocol implementation)
   - src/lora_bridge.cpp/h (LoRa radio management)
   - src/message_router.cpp/h (routing logic and deduplication)
   - src/config_manager.cpp/h (NVS-based configuration)
   - include/bitchat_protocol.h (protocol constants from iOS app)
   - include/hardware_config.h (pin definitions for Heltec)
3. Configure platformio.ini with required libraries (NimBLE, RadioLib, etc.)
4. Ensure the project builds successfully with empty implementations

### Prompt 1.2: BitChat Protocol Constants
**Status: [x] Completed**

Extract and implement the BitChat protocol constants from the iOS app:
1. Read BluetoothMeshService.swift and extract:
   - Service UUID: "12345678-1234-5678-1234-56789abcdef0"
   - RX Characteristic UUID: "12345678-1234-5678-1234-56789abcdef1"
   - TX Characteristic UUID: "12345678-1234-5678-1234-56789abcdef2"
2. Read BinaryProtocol.swift and extract:
   - BitchatPacket structure
   - Message type enums
   - Version negotiation constants (HELLO=0xAA, ACK=0xAB)
3. Create include/bitchat_protocol.h with all constants
4. Ensure all values match iOS implementation exactly

---

## Phase 2: BLE Mesh Implementation

### Prompt 2.1: BLE Peripheral Mode
**Status: [x] Completed**

Implement the BLE peripheral (server) functionality to accept connections from iOS devices:
1. In src/ble_mesh.cpp, implement BLE server using NimBLE
2. Create service with UUID "12345678-1234-5678-1234-56789abcdef0"
3. Add RX characteristic "12345678-1234-5678-1234-56789abcdef1" with WRITE property
4. Add TX characteristic "12345678-1234-5678-1234-56789abcdef2" with NOTIFY property
5. Set up callbacks for RX characteristic to receive data
6. Configure BLE advertising with BitChat service UUID
7. Generate peer ID from MAC address using same format as iOS (8 char hex string)
8. Test that iOS app can discover and connect to the repeater

### Prompt 2.2: BLE Architecture Simplification  
**Status: [x] Completed**

Simplify BLE implementation for iOS-only connections:
1. Remove BLE central/client functionality (repeater-to-repeater via LoRa only)
2. Focus BLE peripheral mode on iOS device connections exclusively
3. Eliminate scanning, peer discovery, and client connection management
4. Streamline data flow: iOS ↔ BLE ↔ Message Router ↔ LoRa Mesh
5. Optimize memory usage and reduce complexity
6. Prepare clean interface for LoRa mesh integration (Phase 8)

### Prompt 2.3: Version Negotiation
**Status: [x] Completed**

Implement the BitChat version negotiation protocol for iOS connections:
1. As peripheral: wait for VERSION_HELLO (0x20) message after iOS connection
2. Parse version from VERSION_HELLO and respond with VERSION_ACK (0x21) + version 1
3. Store negotiated version for each connected iOS device
4. Set connection state to "ready" only after successful negotiation
5. Reject messages from connections without completed negotiation
6. Test with iOS app to ensure version negotiation succeeds
7. Handle version incompatibility gracefully

---

## Phase 3: Message Handling

### Prompt 3.1: BitChat Packet Parser
**Status: [x] Completed**

Implement the BitChat binary packet parser:
1. Create BitchatPacket struct in bitchat_protocol.h matching iOS
2. Implement parsePacket() function to deserialize binary data
3. Implement serializePacket() function to create binary data
4. Support all message types:
   - 0x01: Broadcast message
   - 0x02: Private message  
   - 0x03: Presence update
   - 0x04: File transfer
   - 0x05: Typing indicator
   - 0x06: Delivery ACK
5. Validate packet structure and return errors for malformed data
6. Test parser with sample packets from iOS app

### Prompt 3.2: Message Deduplication
**Status: [x] Completed**

Implement a robust message deduplication system:
1. Create MessageCache class in message_router.cpp
2. Use circular buffer for 1000 most recent message IDs
3. Create hash map for O(1) lookup by message ID
4. Add timestamp to entries and expire after 5 minutes
5. Make thread-safe with mutex protection
6. Add methods:
   - bool isDuplicate(messageId, timestamp)
   - void addMessage(messageId, timestamp)
   - void cleanupExpired()
7. Add unit tests for deduplication logic

### Prompt 3.3: Message Router
**Status: [x] Completed**

Implement the core message routing logic:
1. Create MessageRouter class to handle routing decisions
2. When receiving from BLE (iOS devices):
   - Check deduplication cache
   - Decrement TTL (drop if 0)
   - Queue for LoRa mesh transmission to other repeaters
3. When receiving from LoRa mesh:
   - Check deduplication cache
   - Decrement TTL (drop if 0)
   - Forward to all connected iOS devices via BLE
4. Add 0-100ms random delay for LoRa to prevent collisions
5. Track source to prevent echo and loops
6. Test with multiple repeaters to verify no loops

---

## Phase 4: LoRa Bridge

### Prompt 4.1: LoRa Radio Setup
**Status: [ ] Not Started**

Implement LoRa radio initialization and configuration:
1. In src/lora_bridge.cpp, initialize SX1262 using RadioLib
2. Use Heltec pin definitions from hardware_config.h
3. Configure radio parameters:
   - Frequency: 915.0 MHz (make configurable later)
   - Bandwidth: 125 kHz
   - Spreading Factor: 9
   - Coding Rate: 4/5
   - TX Power: 20 dBm
   - Sync Word: 0x12 (private network)
4. Enable hardware CRC
5. Set up DIO1 interrupt handler for RX
6. Implement startReceive() for continuous RX mode
7. Test basic TX/RX between two devices

### Prompt 4.2: LoRa Packet Format
**Status: [ ] Not Started**

Design a minimal LoRa packet format:
1. Create LoRaPacket struct:
   ```c
   struct LoRaPacket {
       uint8_t magic;        // 0xBC
       uint8_t version;      // 0x01
       uint32_t repeaterId;  // From MAC address
       uint8_t flags;        // Reserved
       uint8_t payloadLen;   // BitChat packet length
       uint8_t payload[220]; // BitChat packet
   };
   ```
2. Implement packet serialization/deserialization
3. Validate magic and version on receive
4. Handle fragmentation for packets > 220 bytes
5. Test packet format between repeaters

### Prompt 4.3: LoRa Transmission
**Status: [ ] Not Started**

Implement LoRa transmission with collision avoidance:
1. Implement transmitPacket() with CAD (Channel Activity Detection)
2. If channel busy, exponential backoff 50-400ms
3. Create transmission queue (max 20 packets)
4. Add duty cycle tracking:
   - Track airtime per hour
   - Enforce 1% duty cycle (36 seconds/hour)
   - Drop packets if limit exceeded
5. Monitor RSSI/SNR and log link quality
6. Test reliability with 3+ repeaters

---

## Phase 5: Optimization & Reliability

### Prompt 5.1: Connection Management
**Status: [ ] Not Started**

Implement intelligent connection management:
1. Create ConnectionManager class
2. Limit connections: 3 as central, unlimited as peripheral
3. Track connection quality:
   - RSSI values
   - Message count
   - Connection duration
   - Error rate
4. Implement connection scoring algorithm
5. Replace poor connections when better options available
6. Add hysteresis to prevent flapping
7. Test with many devices to verify stability

### Prompt 5.2: Message Prioritization
**Status: [ ] Not Started**

Implement message prioritization and queue management:
1. Create priority queues for each message type
2. Priority order: presence > private > broadcast > file
3. Set queue limits:
   - Presence: 10 messages
   - Private: 20 messages
   - Broadcast: 30 messages
   - File: 5 messages
4. Drop oldest when queue full
5. Implement fair queuing between sources
6. Add queue depth monitoring
7. Test under high load conditions

### Prompt 5.3: Power Management
**Status: [ ] Not Started**

Implement power optimization:
1. Use ESP32 light sleep when idle
2. Dynamic BLE advertising:
   - Active: 100ms interval
   - Idle (>30s): 1000ms interval
3. Batch LoRa transmissions when possible
4. Reduce scan window when few devices nearby
5. Monitor battery voltage via ADC
6. Reduce TX power when battery < 20%
7. Add deep sleep on long button press
8. Test battery life in various scenarios

---

## Phase 6: Configuration & Monitoring

### Prompt 6.1: Configuration Storage
**Status: [ ] Not Started**

Implement NVS-based configuration:
1. Create ConfigManager class using ESP32 Preferences library
2. Configuration parameters:
   - deviceName: auto-generated "Repeater-XX"
   - loraRegion: US915/EU868/AS923
   - txPowerLimit: 20 dBm
   - dutyCycleLimit: 1%
   - debugLevel: 0-3
3. Load on boot, create defaults if missing
4. Configuration reset: hold button during boot
5. Add version field for future migrations
6. Test persistence across power cycles

### Prompt 6.2: Status Reporting
**Status: [ ] Not Started**

Implement status reporting via presence messages:
1. Modify presence messages to identify as repeater:
   - Nickname: "[Repeater] XX"
   - Include connection count
   - Include LoRa RSSI if available
2. Send presence every 30 seconds
3. Include firmware version in presence
4. Ensure repeater appears in iOS peer list
5. Test visibility in BitChat app

### Prompt 6.3: Debug Interface
**Status: [ ] Not Started**

Implement serial debug interface:
1. Create SerialDebug class with command parser
2. Implement commands:
   - `status`: Show connections, queues, uptime
   - `stats`: Message counts, dedup hits, errors
   - `reset`: Restart device
   - `config`: Show current configuration
   - `lora`: LoRa radio statistics
   - `ble`: BLE connection details
   - `log <level>`: Set debug level (0-3)
3. Add formatted output with tables
4. Include rolling statistics (msgs/min)
5. Test all commands via serial monitor

---

## Phase 7: Testing & Validation

### Prompt 7.1: Loop Prevention Test
**Status: [ ] Not Started**

Test and verify loop prevention:
1. Add test mode activated by serial command
2. Generate test broadcast every 10 seconds
3. Set up 3+ repeaters in triangle formation
4. Verify each message received exactly once
5. Monitor deduplication hit rate
6. Test with different TTL values (1-10)
7. Document results and any issues found

### Prompt 7.2: Compatibility Test
**Status: [ ] Not Started**

Comprehensive compatibility testing:
1. Test with real iOS BitChat app:
   - Repeater appears as peer
   - Can receive broadcasts
   - Can relay private messages
   - Presence updates work
   - Typing indicators relay
2. Test mixed networks:
   - Phone → Repeater → Phone (single hop via BLE)
   - Phone → Repeater → LoRa Mesh → Repeater → Phone (multi-hop)
   - Multiple phones and repeaters in mesh topology
3. Verify 24-hour stability
4. Document any compatibility issues

### Prompt 7.3: Performance Optimization
**Status: [ ] Not Started**

Final performance optimization:
1. Profile with ESP32 performance counters
2. Identify and optimize hot paths
3. Tune buffer and queue sizes
4. Optimize LoRa parameters:
   - Test SF7 vs SF9 vs SF10
   - Measure range vs reliability
5. Memory usage optimization
6. Add runtime statistics
7. Create performance report
8. Make final adjustments based on results

---

## Phase 8: LoRa Mesh Networking (Meshtastic-Inspired)

### Overview
Implement a lightweight mesh protocol inspired by Meshtastic's proven concepts, but tailored for BitChat compatibility. This allows repeaters to form multi-hop networks while maintaining transparent operation with iOS BitChat apps.

### Key Differences from Meshtastic:
- Maintains BitChat BLE protocol on device-facing side
- Simplified mesh protocol optimized for BitChat message relay
- No encryption on mesh layer (BitChat handles E2E encryption)
- Focused on message relay rather than general-purpose messaging

### Prompt 8.1: Mesh Protocol Design
**Status: [ ] Not Started**

Design and implement a lightweight mesh protocol for repeater-to-repeater communication:
1. Create mesh packet types in lora_bridge.h:
   - NEIGHBOR_ANNOUNCE (0x01): Periodic broadcasts with repeater info
   - ROUTE_REQUEST (0x02): Path discovery messages
   - ROUTE_REPLY (0x03): Path confirmation
   - DATA (0x04): Encapsulated BitChat messages
   - MESH_ACK (0x05): Hop-by-hop acknowledgments
2. Add mesh header to LoRa packets:
   ```c
   struct MeshHeader {
       uint8_t type;          // Mesh packet type
       uint32_t srcRepeater;  // Original repeater ID
       uint32_t destRepeater; // Target repeater (0xFFFFFFFF = broadcast)
       uint32_t nextHop;      // Next hop repeater
       uint8_t hopCount;      // Hops traveled
       uint8_t maxHops;       // TTL for mesh (default 5)
       uint16_t seqNum;       // Sequence number for routing
   };
   ```
3. Modify LoRaPacket to include mesh header
4. Design simple distance-vector routing algorithm
5. Test basic mesh packet exchange between repeaters

### Prompt 8.2: Neighbor Discovery
**Status: [ ] Not Started**

Implement neighbor discovery and maintenance:
1. Create NeighborTable class in lora_bridge.cpp
2. Send NEIGHBOR_ANNOUNCE every 60 seconds containing:
   - Repeater ID and name
   - Current load (connected BLE devices, queue depth)
   - Capabilities flags (battery powered, GPS equipped, etc.)
   - Firmware version
3. Maintain neighbor table with:
   - Neighbor ID
   - Last seen timestamp  
   - Average RSSI (rolling average of last 10 packets)
   - Link quality score (based on RSSI and packet success rate)
   - Hop count to neighbor
4. Remove stale neighbors after 5 minutes of no contact
5. Implement adaptive announcement rate:
   - Every 30s when network unstable
   - Every 120s when stable
6. Add neighbor table to debug interface

### Prompt 8.3: Mesh Routing
**Status: [ ] Not Started**

Implement routing algorithm for multi-hop communication:
1. Create RouteTable class with route entries
2. Implement simplified AODV (Ad hoc On-Demand Distance Vector):
   - Route discovery only when needed
   - Cache routes with 10-minute timeout
   - Use sequence numbers to ensure fresh routes
3. Route discovery process:
   - Broadcast ROUTE_REQUEST with unique request ID
   - Intermediate nodes add themselves to path and forward
   - Destination sends ROUTE_REPLY back along reverse path
   - Build bidirectional routes from discovery
4. Route maintenance:
   - Monitor next-hop reachability via ACKs
   - Mark routes stale on repeated failures
   - Trigger rediscovery for stale routes
5. Fallback to flooding for broadcast messages
6. Test multi-hop routing with 4+ repeaters

### Prompt 8.4: Reliable Delivery
**Status: [ ] Not Started**

Add reliability layer for mesh communication:
1. Implement hop-by-hop acknowledgments:
   - ACK required for unicast DATA packets
   - 3 retries with exponential backoff (100ms, 200ms, 400ms)
   - Mark link failed after 3 failed attempts
2. Alternative path selection:
   - Maintain up to 3 routes per destination
   - Switch to alternate route on failure
   - Load balance across multiple good routes
3. End-to-end delivery for critical messages:
   - Optional E2E ACK for private messages
   - Source retransmission if no E2E ACK
4. Store-and-forward buffer:
   - Hold messages for temporarily unreachable repeaters
   - Maximum 50 messages, 5 minute timeout
5. Mesh-level deduplication:
   - Separate from BitChat dedup
   - Track by source + sequence number

### Prompt 8.5: Mesh Optimization
**Status: [ ] Not Started**

Optimize mesh performance and efficiency:
1. Implement adaptive data rates:
   - Monitor link RSSI to each neighbor
   - Use SF7 for strong links (>-80 dBm)
   - Use SF9 for medium links (-80 to -100 dBm)  
   - Use SF10 for weak links (<-100 dBm)
   - Adjust per-link, not globally
2. Channel coordination:
   - Implement simple TDMA for known neighbors
   - 100ms slots, rotating schedule
   - Fall back to CSMA for new nodes
3. Load balancing:
   - Include queue depth in routing metrics
   - Prefer less loaded paths
   - Distribute broadcasts across time
4. Power-aware routing:
   - Mark battery-powered nodes in announcements
   - Prefer mains-powered repeaters for routing
   - Reduce announce rate for battery nodes
5. Test optimizations with 10+ node network

### Prompt 8.6: Mesh Integration
**Status: [ ] Not Started**

Integrate mesh networking with existing message flow:
1. Modify message_router.cpp to use mesh routing:
   - Check if destination repeater is known
   - Use mesh unicast for targeted delivery
   - Fall back to broadcast for unknown destinations
2. Add mesh statistics to status messages:
   - Number of mesh neighbors
   - Routing table size
   - Mesh reliability metrics
3. Implement hybrid routing:
   - Direct LoRa broadcast for 1-hop neighbors
   - Mesh routing for distant repeaters
   - Automatic mode selection
4. Update debug interface with mesh commands:
   - `mesh`: Show neighbor and route tables
   - `ping <repeater-id>`: Test mesh connectivity
   - `trace <repeater-id>`: Show route path
5. Test full integration with BitChat traffic

---

## Completion Checklist

After all prompts are complete, verify:

- [ ] Repeater appears as normal peer in iOS app
- [ ] Messages flow bidirectionally through repeater
- [ ] No message loops with multiple repeaters
- [ ] TTL properly decremented
- [ ] Deduplication prevents floods
- [ ] LoRa extends range significantly beyond BLE
- [ ] LoRa mesh enables multi-hop repeater networks
- [ ] Mesh routing optimizes for reliability and efficiency
- [ ] Neighbor discovery maintains network topology
- [ ] Power consumption acceptable for battery use
- [ ] Configuration persists across reboots
- [ ] Debug interface provides useful information
- [ ] Code is modular and well-documented
- [ ] All tests pass
- [ ] Performance meets expectations

## Notes for Autonomous Agent

- Each prompt builds on previous work, so complete them in order
- Always test after implementation to ensure functionality
- If a prompt seems already implemented, verify before marking complete
- Commit after each successful prompt completion
- Use clear, descriptive commit messages
- Ask for clarification if any prompt is unclear
- The goal is zero changes to the iOS app - maintain exact protocol compatibility
- Phase 8 adds mesh networking between repeaters while maintaining BLE compatibility