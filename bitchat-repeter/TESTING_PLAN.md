# BitChat Repeater Testing Plan

## 🧪 **Test Environment Setup**

### **Hardware Requirements**
- ✅ 2x Heltec WiFi LoRa 32 V3 (ESP32-S3) gateway nodes
- ✅ 2x iOS devices with BitChat app installed
- 1x Computer with serial monitor access
- LoRa antennas connected to both gateways

### **Pre-Test Setup**
1. **Flash both gateway nodes** with the BitChat repeater firmware
2. **Position gateways** ~50-100m apart (within LoRa range, beyond BLE range)
3. **Connect serial monitor** to one gateway for debugging
4. **Ensure iOS devices** have BitChat app updated and ready

---

## 📋 **Testing Phases**

### **Phase 1: Basic Functionality Tests** ⏱️ *30 minutes*

#### **Test 1.1: Firmware Boot & Configuration**
**Objective:** Verify firmware boots and initializes correctly

**Steps:**
1. Power on Gateway 1 with serial monitor connected
2. Observe boot sequence in serial output
3. Verify all components initialize without errors:
   - ConfigManager loads/creates default config
   - BLE service starts advertising
   - LoRa radio initializes
   - All managers initialize successfully

**Expected Results:**
```
BitChat Repeater starting...
Config Manager: Device name: Repeater-XX
BLE Mesh: Initialized as peer ID: XXXXXXXX
LoRa Bridge: Radio configured successfully
BitChat Repeater ready
```

**Debug Commands to Test:**
```
help          # Verify debug interface works
status        # Check system status
config        # Verify configuration loaded
```

#### **Test 1.2: BLE Discovery & Connection**
**Objective:** Verify iOS devices can discover and connect to repeater

**Steps:**
1. Open BitChat app on iOS Device 1
2. Look for repeater in peer discovery
3. Verify repeater appears with format: `[Repeater] Repeater-XX`
4. Attempt to connect to repeater
5. Monitor serial output for connection events

**Expected Results:**
- Repeater appears in iOS peer list
- Connection establishes successfully
- Serial shows: `BLE connection established from iOS device`
- Status updates show presence messages being sent

**Debug Commands:**
```
ble           # Check BLE connection status
looptest      # Verify no test active initially
```

#### **Test 1.3: Version Negotiation**
**Objective:** Verify BLE protocol version negotiation works

**Steps:**
1. With iOS Device 1 connected, monitor serial output
2. Look for VERSION_HELLO and VERSION_ACK messages
3. Verify connection becomes "ready" after negotiation

**Expected Results:**
```
BLE Mesh: Received VERSION_HELLO from peer XXXXXXXX, protocol version 1
BLE Mesh: Sent VERSION_ACK, connection ready
```

---

### **Phase 2: Single Repeater Message Flow** ⏱️ *45 minutes*

#### **Test 2.1: Broadcast Message Reception**
**Objective:** Verify repeater receives and processes broadcast messages from iOS

**Steps:**
1. Connect iOS Device 1 to Gateway 1
2. Send broadcast message from iOS Device 1: `"Test broadcast message"`
3. Monitor serial output for message reception
4. Use debug command to check message statistics

**Expected Results:**
```
Message Router: Processing BLE message type 0x04 from connection 1
Message Router: Queued BLE message for LoRa transmission
```

**Debug Commands:**
```
stats         # Check messages sent/received counts
mesh          # Verify no neighbors yet (single repeater)
```

#### **Test 2.2: Status Reporting (Presence)**
**Objective:** Verify repeater appears as active peer with status updates

**Steps:**
1. Wait 30 seconds with iOS Device 1 connected
2. Observe presence updates in BitChat app
3. Verify repeater status information is displayed
4. Check serial output for status reporting

**Expected Results:**
- iOS app shows repeater as active peer
- Status format: `[Repeater] Name | FW:1.0.0 | BLE:1 | Mesh:0 | Up:XXmXXs | TX:X/RX:X`
- Serial shows: `Status Reporter: Status update sent`

#### **Test 2.3: Message Deduplication**
**Objective:** Verify message deduplication prevents loops

**Steps:**
1. Send same message multiple times rapidly from iOS Device 1
2. Monitor serial output for deduplication behavior
3. Check statistics for duplicate detection

**Expected Results:**
```
Message Router: Dropping duplicate BLE message (ID: 0xXXXXXXXX)
```

**Debug Commands:**
```
stats         # Check dedup hit rate
```

---

### **Phase 3: Dual Gateway Mesh Networking** ⏱️ *60 minutes*

#### **Test 3.1: LoRa Neighbor Discovery**
**Objective:** Verify gateways discover each other via LoRa mesh

**Steps:**
1. Power on Gateway 2 (place ~50-100m from Gateway 1)
2. Wait 2-3 minutes for neighbor discovery
3. Check neighbor tables on both gateways via serial

**Expected Results:**
```
LoRa Bridge: Received NEIGHBOR_ANNOUNCE from XXXXXXXX
Neighbor added: XXXXXXXX (RSSI: -XX dBm)
```

**Debug Commands:**
```
mesh          # Should show 1 neighbor on each gateway
lora          # Check LoRa statistics and RSSI values
```

#### **Test 3.2: Route Discovery**
**Objective:** Verify AODV routing protocol works between gateways

**Steps:**
1. From Gateway 1 serial console, ping Gateway 2:
   ```
   ping XXXXXXXX    # Use Gateway 2's ID from mesh command
   ```
2. Monitor route discovery process
3. Verify bidirectional routes are established

**Expected Results:**
```
Route found: X hops via XXXXXXXX
Route quality: XX%, last used: XXX ms ago
```

#### **Test 3.3: LoRa Mesh Message Relay**
**Objective:** Verify messages relay between gateways via LoRa

**Steps:**
1. Connect iOS Device 1 to Gateway 1
2. Connect iOS Device 2 to Gateway 2 (ensure devices are far apart)
3. Send broadcast from iOS Device 1: `"Mesh test message"`
4. Verify iOS Device 2 receives the message
5. Monitor both gateway serial outputs

**Expected Results:**
- Message appears on iOS Device 2
- Gateway 1 serial: `Message Router: Queued BLE message for LoRa`
- Gateway 2 serial: `Message Router: Processing LoRa message, forwarding to BLE`

---

### **Phase 4: Multi-Device Scenarios** ⏱️ *45 minutes*

#### **Test 4.1: Multiple iOS Connections**
**Objective:** Test multiple iOS devices on single repeater

**Steps:**
1. Connect both iOS devices to Gateway 1
2. Send messages between the iOS devices through the repeater
3. Monitor connection management and message routing

**Expected Results:**
- Both devices appear connected in debug interface
- Messages route correctly between devices
- Connection quality tracking works for multiple connections

**Debug Commands:**
```
ble           # Should show 2 connections
status        # Should show BLE Clients: 2
```

#### **Test 4.2: Cross-Mesh Communication**
**Objective:** Test communication across the full mesh network

**Setup:**
- iOS Device 1 ↔ Gateway 1 ↔ LoRa Mesh ↔ Gateway 2 ↔ iOS Device 2

**Steps:**
1. Position iOS devices so they can only communicate via mesh
2. Send private message from iOS Device 1 to iOS Device 2
3. Send broadcast messages and verify delivery
4. Monitor hop count and routing decisions

**Expected Results:**
- Messages successfully traverse the full path
- Proper hop count tracking (should be 1 hop via LoRa)
- No message loops or duplicates

---

### **Phase 5: Reliability & Edge Cases** ⏱️ *60 minutes*

#### **Test 5.1: Loop Prevention**
**Objective:** Verify loop prevention works with multiple paths

**Steps:**
1. Set up triangle topology: iOS ↔ Gateway 1 ↔ Gateway 2 ↔ Gateway 1
2. Start loop prevention test:
   ```
   looptest start 5 300
   ```
3. Monitor test results for 5 minutes
4. Check final results

**Expected Results:**
```
✓ PASS: No message loops detected!
✓ PASS: No duplicate messages detected!
```

**Debug Commands:**
```
looptest results    # Show pass/fail summary
looptest detailed   # Show per-message analysis
```

#### **Test 5.2: Gateway Failure Recovery**
**Objective:** Test mesh resilience to node failures

**Steps:**
1. Establish communication: iOS 1 ↔ GW1 ↔ GW2 ↔ iOS 2
2. Power off Gateway 1 suddenly
3. Wait for route timeout (~10 minutes)
4. Verify iOS Device 1 can still communicate via alternative paths
5. Power Gateway 1 back on and verify recovery

**Expected Results:**
- Initial communication loss when Gateway 1 fails
- Route discovery finds alternative paths
- Communication resumes when Gateway 1 returns

#### **Test 5.3: Message Prioritization**
**Objective:** Verify message priority queuing works under load

**Steps:**
1. Generate high message load from both iOS devices
2. Mix message types: broadcasts, private messages, presence updates
3. Monitor queue statistics and priority handling

**Expected Results:**
- Presence messages processed first
- Private messages prioritized over broadcasts
- Fair queuing prevents source monopolization

**Debug Commands:**
```
stats         # Check queue depths and message rates
```

---

### **Phase 6: Power & Performance** ⏱️ *30 minutes*

#### **Test 6.1: Power Management**
**Objective:** Verify power optimization features work

**Steps:**
1. Monitor power consumption with no iOS connections
2. Connect iOS device and monitor power changes
3. Test deep sleep functionality (long button press)
4. Monitor battery level reporting

**Expected Results:**
- Lower power consumption when idle
- Adaptive BLE advertising intervals
- Proper sleep mode operation

**Debug Commands:**
```
status        # Check power state and battery level
```

#### **Test 6.2: Performance Under Load**
**Objective:** Test system performance with high message rates

**Steps:**
1. Send rapid message bursts from both iOS devices
2. Monitor memory usage and queue depths
3. Check for message drops or system instability
4. Verify adaptive LoRa parameters adjust correctly

**Expected Results:**
- System remains stable under load
- Adaptive features respond to conditions
- Memory usage stays within limits

**Debug Commands:**
```
status        # Check free heap and system health
lora          # Check adaptive spreading factor changes
```

---

### **Phase 7: 24-Hour Stability Test** ⏱️ *24 hours*

#### **Test 7.1: Long-Term Stability**
**Objective:** Verify 24-hour continuous operation

**Setup:**
1. Leave both gateways running with iOS devices connected
2. Generate periodic test messages (every 5-10 minutes)
3. Monitor system health periodically

**Monitoring Schedule:**
- **Hour 1, 6, 12, 18, 24**: Check system status and statistics
- **Monitor for**: Memory leaks, connection drops, mesh failures

**Success Criteria:**
- Both gateways remain operational for full 24 hours
- No significant memory leaks (heap usage stable)
- Mesh network maintains connectivity
- Message delivery remains reliable

---

## 📊 **Test Results Documentation**

### **Test Results Template**
For each test, document:

```
Test: [Test Name]
Date: [Date/Time]
Duration: [Minutes]
Result: PASS/FAIL
Notes: 
- [Any observations]
- [Performance metrics]
- [Issues encountered]

Serial Output Snippet:
[Key log messages]

Statistics:
Messages Sent: X
Messages Received: X
Neighbors: X
Routes: X
Memory Usage: X KB
```

### **Critical Failure Criteria**
Stop testing and investigate if:
- ❌ Gateway fails to boot or initialize
- ❌ iOS devices cannot discover/connect to repeater
- ❌ Messages are not delivered across mesh
- ❌ System crashes or reboots unexpectedly
- ❌ Memory usage exceeds 80% of available RAM
- ❌ Loop prevention test fails

### **Success Criteria Summary**
The implementation is validated when:
- ✅ All Phase 1-6 tests pass
- ✅ 24-hour stability test completes successfully
- ✅ No critical failures encountered
- ✅ Message delivery reliability >95%
- ✅ Mesh network maintains connectivity under normal conditions

---

## 🛠️ **Troubleshooting Guide**

### **Common Issues & Solutions**

**Gateway won't boot:**
- Check power supply and connections
- Verify firmware flash was successful
- Monitor serial output for error messages

**iOS can't discover repeater:**
- Check BLE advertising is active: `ble` command
- Verify service UUID matches iOS app expectations
- Restart BitChat app and retry discovery

**No LoRa communication:**
- Check antenna connections
- Verify frequency settings match (915MHz US)
- Check for interference or range issues
- Monitor RSSI values: `lora` command

**Message delivery failures:**
- Check mesh routing: `mesh` command
- Verify TTL values are sufficient
- Monitor for deduplication issues: `stats` command
- Check queue depths for congestion

**Memory or performance issues:**
- Monitor heap usage: `status` command
- Check for message queue buildups
- Verify cleanup routines are running
- Consider reducing message load

---

## 📋 **Quick Test Checklist**

### **30-Minute Smoke Test** (Essential functionality)
- [ ] Both gateways boot successfully
- [ ] iOS devices discover and connect to repeaters  
- [ ] Broadcast messages work in both directions
- [ ] LoRa mesh discovers neighbors
- [ ] Messages relay across gateways
- [ ] Debug interface responds to all commands

### **2-Hour Full Test** (Comprehensive validation)
- [ ] All Phase 1-4 tests complete successfully
- [ ] Loop prevention test passes
- [ ] Power management features work
- [ ] Performance under load is acceptable
- [ ] No critical issues encountered

### **24-Hour Production Test** (Deployment readiness)
- [ ] All previous tests pass
- [ ] 24-hour stability test completes
- [ ] No memory leaks or degradation
- [ ] Mesh network remains stable
- [ ] Ready for production deployment

**Good luck with your testing! 🚀**