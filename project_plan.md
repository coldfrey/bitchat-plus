# BitChat LoRa Gateway/Repeater Project Plan (ESP32-S3 + iOS)

**Goal:** Extend BitChat's offline reach by adding *Gateway* repeaters (Heltec ESP32-S3 + SX1262) that bridge BitChat BLE frames over LoRa. Phones connect to a nearby Gateway via BLE; Gateways flood frames over LoRa with TTL+de-dup; receiving Gateways rebroadcast to their local phones over BLE. We will modify the **iOS** app to support this *gateway-specific* transport (no app changes to BitChat's native peer-to-peer needed).

---

## 0) High-Level Architecture

```
[Phone A (BitChat iOS)] --(BLE GATT)--> [Gateway X (ESP32-S3)]
                                              |
                 <---------------- LoRa Flood --------------->
                                              |
[Phone B (BitChat iOS)] <--(BLE GATT)-- [Gateway Y (ESP32-S3)]
```

- **BLE side:** Gateway exposes a *Gateway Service* with TX (phone→GW), RX (GW→phone), Config, and Stats characteristics.
- **LoRa side:** Gateways flood opaque BitChat frames wrapped in a compact *Bridge Header* (TTL, msg_id, frag info, gw_id, hop metrics).
- **Loop control:** TTL decrement + de-dup cache (ring/Bloom) on Gateways; iOS also de-dups locally.
- **Privacy:** Gateways advertise rolling ephemeral IDs (EID) in BLE advertising Service Data to reduce tracking.
- **Phases:** 
  - P1 (this plan): Gateway-specific transport; iOS app gains *GatewayTransport* alongside existing direct BLE transport.
  - P2 (later): Cloudless directory + OTA, adaptive LoRa params, multi-radio mesh.

---

## 1) Repository & Project Layout

> Current BitChat repository structure with proposed additions for the Gateway feature.

```yaml
/bitchat                           # Repository root
  /bitchat/                        # iOS/macOS app source
    /Assets.xcassets/              # App icons and assets
    /Identity/                     # Identity management
      IdentityModels.swift
      SecureIdentityStateManager.swift
    /Noise/                        # Noise Protocol implementation
      NoiseHandshakeCoordinator.swift
      NoiseProtocol.swift
      NoiseSecurityConsiderations.swift
      NoiseSession.swift
    /Protocols/                    # BitChat protocol
      BinaryEncodingUtils.swift
      BinaryProtocol.swift
      BitchatProtocol.swift
    /Services/                     # Core services
      BluetoothMeshService.swift
      DeliveryTracker.swift
      KeychainManager.swift
      MessageRetryService.swift
      NoiseEncryptionService.swift
      NotificationService.swift
    /Utils/                        # Utilities
      BatteryOptimizer.swift
      CompressionUtil.swift
      LRUCache.swift
      OptimizedBloomFilter.swift
      SecureLogger.swift
    /ViewModels/                   # View models
      ChatViewModel.swift
    /Views/                        # SwiftUI views
      AppInfoView.swift
      ContentView.swift
      FingerprintView.swift
      LinkPreviewView.swift
    /GatewayKit/                   # NEW: Gateway transport package
      Package.swift
      Sources/GatewayKit/
        GatewayTransport.swift
        GatewayPeripheral.swift
        BridgeHeader.swift
        GatewayStats.swift
      Tests/GatewayKitTests/
    BitchatApp.swift               # App entry point
    Info.plist
    bitchat.entitlements
    bitchat-macOS.entitlements
    LaunchScreen.storyboard
  
  /bitchat-repeter/                # ESP32 repeater firmware (existing)
    platformio.ini                 # PlatformIO config
    pyproject.toml
    flash_both.sh
    /src/                          # NEW: Gateway firmware source
      main.cpp
      ble_gateway.cpp/.h
      lora_radio.cpp/.h
      router.cpp/.h
      eid.cpp/.h
      config.cpp/.h
      storage.cpp/.h
      stats.cpp/.h
    /include/                      # NEW: Headers
      bridge.h
      hw_pins.h
    /lib/                          # Libraries
    /main/                         # Existing ESP-IDF main
      CMakeLists.txt
    /test/                         # Unit tests
    /.pio/                         # PlatformIO build
  
  /bitchatShareExtension/          # iOS share extension
    ShareViewController.swift
    Info.plist
    bitchatShareExtension.entitlements
  
  /bitchatTests/                   # Test suite
    /EndToEnd/
    /Integration/
    /Mocks/
    /Noise/
    /Protocol/
    /TestUtilities/
  
  /bitchat.xcodeproj/              # Xcode project
  
  # Root files
  AI_CONTEXT.md                    # AI assistant context
  BRING_THE_NOISE.md               # Noise Protocol documentation
  Justfile                         # Build automation
  LICENSE                          # Public domain
  Package.swift                    # Swift Package Manager
  PRIVACY_POLICY.md
  README.md
  WHITEPAPER.md                    # Protocol specification
  project.yml                      # XcodeGen config
  project_plan.md                  # This document
  setup.sh                         # Setup script
```

---

## 2) Protocol Primitives

### 2.1 UUIDs (fixed and public)
> Use static random 128-bit UUIDs (generated once) to ensure cross-platform stability.

```c
GATEWAY_SERVICE_UUID     = "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001"
GW_CHAR_TX_UUID         = "7A1B1001-6B2E-46E8-8D2D-6AA2E5A4F001"  // WRITE_NR (Phone->GW)
GW_CHAR_RX_UUID         = "7A1B1002-6B2E-46E8-8D2D-6AA2E5A4F001"  // NOTIFY (GW->Phone)
GW_CHAR_CFG_UUID        = "7A1B2001-6B2E-46E8-8D2D-6AA2E5A4F001"  // READ/WRITE
GW_CHAR_STATS_UUID      = "7A1B2002-6B2E-46E8-8D2D-6AA2E5A4F001"  // READ/NOTIFY
BLE_AD_SERVICE_UUID     = GATEWAY_SERVICE_UUID
```

### 2.2 BLE MTU & Frame Size
- Target iOS negotiated MTU ≈ 185–247; write in chunks ≤ 180 bytes for safety.
- RX characteristic notifications also ≤ 180 bytes.
- Implement fragmentation at the *Gateway header* layer for both BLE and LoRa.

### 2.3 Bridge Header (little-endian)
```c
struct BridgeHdr {
    uint16_t magic;         // 0xBC77
    uint8_t  ver;           // 0x01
    uint8_t  ttl;           // start 6 (configurable), decrement per LoRa hop
    uint64_t msg_id;        // randomly generated by sender (phone or GW)
    uint32_t gw_id;         // sender gateway identifier
    uint8_t  frag_idx;      // 0..(frag_total-1)
    uint8_t  frag_total;    // 1..N
    uint16_t payload_len;   // bytes following header in this fragment
    uint16_t hop;           // hop count seen so far (increment on forward)
    uint16_t crc16;         // CRC-16-IBM over header(with crc16=0) + payload
} __attribute__((packed));  // total header: 24 bytes
```

- **Opaque payload:** A BitChat BLE frame (whatever the app uses now). Gateways are content-agnostic.
- **De-dup key:** `(msg_id, frag_idx, frag_total)`; cache full msgs until all frags received or timeout.

### 2.4 BLE Advertising Service Data (rolling EID)
```
EID = Truncate_6(HMAC-SHA256(gw_secret, floor(unix_time / 60)))
Service Data: [ 0..5 = EID ][ 6 = ver ][ 7 = region_code ][ 8 = sf ][ 9 = bw ][ 10 = tx_dBm ]
```

- `gw_secret`: 16 bytes stored in NVS; generated first boot if absent.
- iOS can display Gateways and basic radio params without connecting.

### 2.5 LoRa Radio Defaults (configurable at runtime)
- Region: EU868 (default), BW=125kHz, SF=9, CR=4/5, Preamble=8, TX power within local limits.
- On SX1262 via RadioLib.

---

## 3) Firmware (ESP32-S3) — Design

### 3.1 Modules
- `ble_gateway.*` — NimBLE server, Service/Chars, advertising with EID, fragmentation/assembly for BLE side.
- `lora_radio.*` — RadioLib init, send/receive with CAD, duty cycle guard.
- `router.*` — Core bridge logic: TTL decrement, de-dup store, frag reassembly, RX/TX queues, flood control.
- `eid.*` — HMAC(EID) and 60-sec ticker.
- `config.*` — In-RAM config object + NVS persistence; defaults + validation.
- `storage.*` — NVS helpers (gw_id, secret, region).
- `stats.*` — Counters, moving averages, exposed via STAT char.
- `hw_pins.h` — SX1262 and board pins.

### 3.2 RTOS Tasks & Queues
```
Task: BLE_RX    -> pushes inbound BLE fragments to Router
Task: Router    -> de-dup + TTL + fragment/defrag; fanout to BLE_TX and LORA_TX
Task: LoRa_RX   -> pushes inbound LoRa fragments to Router
Task: BLE_TX    -> notifies phones with assembled frames / fragments
Task: LoRa_TX   -> sends fragments on radio with backoff
Task: EID_Rotate -> updates advert EID each 60s
```

- Queues: `q_ble_in`, `q_ble_out`, `q_lora_in`, `q_lora_out`.
- De-dup cache: 1024 entries ring (LRU) or Bloom filter + small hashmap for partials; expiry 60s.

### 3.3 Hardware Pins (Heltec WiFi LoRa 32 V3 — ESP32-S3 + SX1262)
```cpp
// hw_pins.h
#define LORA_CS   8
#define LORA_SCK  9
#define LORA_MOSI 10
#define LORA_MISO 11
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
```

### 3.4 Config Struct (persisted)
```cpp
struct GwConfig {
    uint32_t gw_id;          // random on first boot
    uint8_t  region;         // 0=EU868,1=US915,2=AS923...
    uint8_t  sf;             // 7..12
    uint8_t  bw;             // 0=125k,1=250k,2=500k
    int8_t   tx_dbm;         // within legal bounds
    uint8_t  ttl;            // default 6
    uint8_t  ble_adv_int_ms; // e.g., 200
    uint8_t  log_level;      // 0..3
    uint8_t  _resv;
    uint8_t  gw_secret[16];  // for EID
} __attribute__((packed));
```

### 3.5 BLE GATT (Server)
- **TX (Write Without Response):** phone→GW (fragments with BridgeHdr). Max write len ~180 bytes.
- **RX (Notify):** GW→phone (fragments).
- **CFG (Read/Write):** binary struct GwConfig (excluding gw_secret on read; allow specific fields write).
- **STATS (Read/Notify):** counters {uptime_s, lora_rx, lora_tx, ble_rx, ble_tx, dedup_hits, crc_err, duty_block_ms}.

### 3.6 Firmware Code Skeletons

#### src/main.cpp
```cpp
#include "ble_gateway.h"
#include "lora_radio.h"
#include "router.h"
#include "config.h"
#include "eid.h"
#include "stats.h"

void setup() {
    Serial.begin(115200);
    Config::init();         // loads NVS or sets defaults; generates gw_id/secret if missing
    Stats::init();
    LoRaRadio::init(Config::cur());
    EID::init(Config::cur());
    BLEGateway::init(Config::cur());  // starts advertising with EID
    Router::init();
}

void loop() { 
    vTaskDelay(pdMS_TO_TICKS(1000)); 
}
```

#### src/router.cpp (core routing)
```cpp
#include "router.h"
#include "stats.h"
#include "lora_radio.h"
#include "ble_gateway.h"
#include <unordered_map>

namespace Router {
    static QueueHandle_t q_ble_in, q_lora_in, q_ble_out, q_lora_out;

    struct FragKey { 
        uint64_t msg_id; 
        uint8_t frag_idx, frag_total; 
    };
    
    struct FragKeyHash { 
        size_t operator()(FragKey const& k) const {
            return (size_t)(k.msg_id ^ ((uint64_t)k.frag_idx<<56) ^ ((uint64_t)k.frag_total<<48));
        }
    };
    
    static std::unordered_map<uint64_t, uint32_t> dedup_ring; // msg_id -> timestamp

    void init() {
        q_ble_in  = xQueueCreate(64, sizeof(Frame));
        q_lora_in = xQueueCreate(64, sizeof(Frame));
        q_ble_out = xQueueCreate(64, sizeof(Frame));
        q_lora_out= xQueueCreate(64, sizeof(Frame));
        xTaskCreate(taskRouter, "router", 8192, nullptr, 5, nullptr);
    }

    static bool seen(uint64_t msg_id) {
        auto it = dedup_ring.find(msg_id);
        if (it != dedup_ring.end()) return true;
        if (dedup_ring.size() > 2048) dedup_ring.clear();
        dedup_ring[msg_id] = (uint32_t)millis();
        return false;
    }

    static void forward(Frame& f, bool from_ble) {
        if (f.hdr.ttl == 0) return;
        if (seen(f.hdr.msg_id)) { 
            Stats::dedup_hits++; 
            return; 
        }
        f.hdr.ttl--; 
        f.hdr.hop++;
        // fanout
        if (from_ble) { 
            xQueueSend(q_lora_out, &f, 0); 
            xQueueSend(q_ble_out, &f, 0); 
        }
        else { 
            xQueueSend(q_ble_out, &f, 0); 
            xQueueSend(q_lora_out, &f, 0); 
        }
    }

    void taskRouter(void*) {
        Frame f;
        for (;;) {
            if (xQueueReceive(q_ble_in, &f, pdMS_TO_TICKS(5)) == pdTRUE) 
                forward(f, true);
            if (xQueueReceive(q_lora_in,&f, pdMS_TO_TICKS(5)) == pdTRUE) 
                forward(f, false);
            // drain TX queues
            while (xQueueReceive(q_ble_out, &f, 0) == pdTRUE)   
                BLEGateway::notify(f);
            while (xQueueReceive(q_lora_out,&f, 0) == pdTRUE)   
                LoRaRadio::send(f);
        }
    }

    // Inbound entry points:
    void onBleFrame(const Frame& f)  { 
        xQueueSend(q_ble_in,  &f, 0); 
        Stats::ble_rx++; 
    }
    
    void onLoRaFrame(const Frame& f) { 
        xQueueSend(q_lora_in, &f, 0); 
        Stats::lora_rx++; 
    }
}
```

#### src/ble_gateway.cpp (fragmentation/notify)
```cpp
#include "ble_gateway.h"
#include "router.h"
#include "config.h"
#include "eid.h"
#include <NimBLEDevice.h>

static NimBLEServer *server;
static NimBLECharacteristic *chTX, *chRX, *chCFG, *chSTAT;

namespace BLEGateway {
    static GwConfig cfg;

    void init(const GwConfig& c) {
        cfg = c;
        NimBLEDevice::init("BC-GW");
        NimBLEServer *s = NimBLEDevice::createServer();
        auto svc = s->createService(GATEWAY_SERVICE_UUID);

        chTX = svc->createCharacteristic(GW_CHAR_TX_UUID, NIMBLE_PROPERTY::WRITE_NR);
        chRX = svc->createCharacteristic(GW_CHAR_RX_UUID, NIMBLE_PROPERTY::NOTIFY);
        chCFG= svc->createCharacteristic(GW_CHAR_CFG_UUID,
              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
        chSTAT=svc->createCharacteristic(GW_CHAR_STATS_UUID,
              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

        chTX->setCallbacks(new class : public NimBLECharacteristicCallbacks {
            void onWrite(NimBLECharacteristic* c) override {
                std::string v = c->getValue();
                if (v.size() < sizeof(BridgeHdr)) return;
                Frame f; 
                memcpy(&f.hdr, v.data(), sizeof(BridgeHdr));
                memcpy(f.data, v.data()+sizeof(BridgeHdr), v.size()-sizeof(BridgeHdr));
                f.len = v.size()-sizeof(BridgeHdr);
                Router::onBleFrame(f);
            }
        });

        svc->start();
        
        // Advertise with rolling EID
        auto adv = NimBLEDevice::getAdvertising();
        adv->addServiceUUID(BLE_AD_SERVICE_UUID);
        uint8_t sd[11]; 
        EID::fillServiceData(sd, sizeof(sd)); // 6 EID + 5 params
        adv->setServiceData(BLE_AD_SERVICE_UUID, std::string((char*)sd, sizeof(sd)));
        adv->setMinInterval(cfg.ble_adv_int_ms/0.625);
        adv->setMaxInterval(cfg.ble_adv_int_ms/0.625);
        adv->start();
    }

    void notify(const Frame& f) {
        std::string out(sizeof(BridgeHdr)+f.len, '\0');
        memcpy(out.data(), &f.hdr, sizeof(BridgeHdr));
        memcpy(out.data()+sizeof(BridgeHdr), f.data, f.len);
        chRX->setValue(out);
        chRX->notify();
    }
}
```

#### src/lora_radio.cpp (RadioLib)
```cpp
#include "lora_radio.h"
#include "router.h"
#include "stats.h"
#include <RadioLib.h>
#include "hw_pins.h"

static SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

namespace LoRaRadio {
    static GwConfig cfg;

    void init(const GwConfig& c) {
        cfg = c;
        int state = radio.begin(868.0, 
                                cfg.bw==0?125.0:cfg.bw==1?250.0:500.0, 
                                cfg.sf, 5, 8, false);
        radio.setCRC(true);
        radio.setOutputPower(cfg.tx_dbm);
        radio.setDio2AsRfSwitch(true);
        radio.startReceive();
        xTaskCreate(rxTask, "lrx", 4096, nullptr, 5, nullptr);
    }

    void rxTask(void*) {
        for (;;) {
            if (radio.available() > 0) {
                uint8_t buf[256]; 
                int len = radio.readData(buf, sizeof(buf));
                if (len >= (int)sizeof(BridgeHdr)) {
                    Frame f; 
                    memcpy(&f.hdr, buf, sizeof(BridgeHdr));
                    f.len = len - sizeof(BridgeHdr);
                    memcpy(f.data, buf+sizeof(BridgeHdr), f.len);
                    Router::onLoRaFrame(f);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    void send(const Frame& f) {
        uint8_t buf[256];
        int len = sizeof(BridgeHdr)+f.len;
        memcpy(buf, &f.hdr, sizeof(BridgeHdr));
        memcpy(buf+sizeof(BridgeHdr), f.data, f.len);
        radio.standby();
        radio.transmit(buf, len);
        radio.startReceive();
        Stats::lora_tx++;
    }
}
```

> **Note:** Real code should include CAD/backoff, duty-cycle limiter per region, and CRC/HMAC checks.

---

## 4) iOS (Swift) — App Modifications

### 4.1 Design Overview
Introduce **GatewayKit** (Swift Package) with:

- **GatewayCentral** (CoreBluetooth central manager)
- **GatewayPeripheral** (session to a single Gateway: connect, negotiate MTU, RX notify, WRITE_NR)
- **GatewayTransport** (BitChat transport interface: send/receive frames; de-dup; merge with existing)
- **PresenceService** (small app-layer presence beacons, optional)
- **LoRaDirectory** (optional, local cache keyed by gw_id + coarse geohash)
- **StatsView & GatewayMapView** (SwiftUI helpers)

**Background support:**
- Enable `Uses Bluetooth LE accessories` and `Background Modes: Uses Bluetooth LE accessories`.
- Use State Preservation/Restoration for CBCentral.

**Security:**
- All user messages remain E2E at BitChat layer; Gateways do not decrypt.

### 4.2 Public API (GatewayKit)

#### Sources/GatewayKit/GatewayTransport.swift
```swift
import Foundation
import CoreBluetooth

public struct BridgeHeader: Codable {
    public var magic: UInt16      // 0xBC77
    public var ver: UInt8         // 0x01
    public var ttl: UInt8
    public var msgId: UInt64
    public var gwId: UInt32
    public var fragIdx: UInt8
    public var fragTotal: UInt8
    public var payloadLen: UInt16
    public var hop: UInt16
    public var crc16: UInt16
}

public protocol BitChatTransport {
    func start() async
    func stop()
    func sendOpaque(_ data: Data) async throws
    var frames: AsyncStream<Data> { get }
}

public final class GatewayTransport: NSObject, BitChatTransport {
    public static let serviceUUID = CBUUID(string: "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001")
    public static let txUUID      = CBUUID(string: "7A1B1001-6B2E-46E8-8D2D-6AA2E5A4F001")
    public static let rxUUID      = CBUUID(string: "7A1B1002-6B2E-46E8-8D2D-6AA2E5A4F001")
    public static let cfgUUID     = CBUUID(string: "7A1B2001-6B2E-46E8-8D2D-6AA2E5A4F001")

    private let central = CBCentralManager(delegate: nil, queue: .main, options: [
        CBCentralManagerOptionShowPowerAlertKey: true,
        CBCentralManagerOptionRestoreIdentifierKey: "io.bitchat.gateway.central"
    ])

    private var streamCont: AsyncStream<Data>.Continuation?
    public  private(set) var frames: AsyncStream<Data>
    private var current: GatewayPeripheral?

    public override init() {
        var cont: AsyncStream<Data>.Continuation!
        self.frames = AsyncStream<Data> { c in cont = c }
        self.streamCont = cont
        super.init()
        self.central.delegate = self
    }

    public func start() async {
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: [Self.serviceUUID],
                                       options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        }
    }
    
    public func stop() {
        central.stopScan()
        if let p = current { 
            central.cancelPeripheralConnection(p.peripheral) 
        }
    }

    public func sendOpaque(_ data: Data) async throws {
        guard let p = current else { 
            throw NSError(domain: "GW", code: 1) 
        }
        try await p.sendBitChatFrame(data)
    }

    // Internal: push frames
    fileprivate func onOpaqueFrame(_ data: Data) { 
        streamCont?.yield(data) 
    }
}

extension GatewayTransport: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: [Self.serviceUUID], options: [
                CBCentralManagerScanOptionAllowDuplicatesKey: true
            ])
        }
    }
    
    public func centralManager(_ central: CBCentralManager,
                               didDiscover peripheral: CBPeripheral,
                               advertisementData: [String : Any],
                               rssi RSSI: NSNumber) {
        // Choose the best peripheral by RSSI / not connected yet
        guard current == nil else { return }
        current = GatewayPeripheral(peripheral: peripheral, manager: self)
        central.connect(peripheral, options: nil)
    }
    
    public func centralManager(_ central: CBCentralManager, 
                               didConnect peripheral: CBPeripheral) {
        peripheral.delegate = current
        peripheral.discoverServices([Self.serviceUUID])
    }
    
    public func centralManager(_ central: CBCentralManager,
                               didDisconnectPeripheral peripheral: CBPeripheral, 
                               error: Error?) {
        current = nil
        // Resume scanning to reattach
        central.scanForPeripherals(withServices: [Self.serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }
}
```

#### Sources/GatewayKit/GatewayPeripheral.swift
```swift
import Foundation
import CoreBluetooth
import CryptoKit

final class GatewayPeripheral: NSObject, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    unowned let manager: GatewayTransport

    private var chTX: CBCharacteristic?
    private var chRX: CBCharacteristic?
    private var mtu: Int = 180
    private var dedup = LruSet<UInt64>(capacity: 2048)

    init(peripheral: CBPeripheral, manager: GatewayTransport) {
        self.peripheral = peripheral
        self.manager = manager
        super.init()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil else { return }
        for s in peripheral.services ?? [] where s.uuid == GatewayTransport.serviceUUID {
            peripheral.discoverCharacteristics([GatewayTransport.txUUID,
                                                GatewayTransport.rxUUID,
                                                GatewayTransport.cfgUUID], for: s)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, 
                    didDiscoverCharacteristicsFor service: CBService, 
                    error: Error?) {
        for c in service.characteristics ?? [] {
            if c.uuid == GatewayTransport.txUUID { chTX = c }
            if c.uuid == GatewayTransport.rxUUID { 
                chRX = c
                peripheral.setNotifyValue(true, for: c) 
            }
        }
        // Determine MTU: iOS doesn't expose directly; estimate from maximum write length
        if let tx = chTX {
            mtu = peripheral.maximumWriteValueLength(for: .withoutResponse)
            // ready to send/receive
        }
    }

    func peripheral(_ peripheral: CBPeripheral, 
                    didUpdateValueFor characteristic: CBCharacteristic, 
                    error: Error?) {
        guard error == nil, 
              characteristic.uuid == GatewayTransport.rxUUID,
              let v = characteristic.value, 
              v.count >= 24 else { return }
              
        let hdr = v.withUnsafeBytes { $0.load(as: BridgeHeader.self) }
        if hdr.magic != 0xBC77 { return }
        if dedup.contains(hdr.msgId) { return }
        
        // NOTE: For simplicity we pass through fragments to upper layer assuming small frames;
        // production should reassemble (frag_total>1) before yielding.
        dedup.insert(hdr.msgId)
        let payload = v.suffix(from: 24)
        manager.onOpaqueFrame(Data(payload))
    }

    func sendBitChatFrame(_ data: Data) async throws {
        guard let tx = chTX else { 
            throw NSError(domain: "GW", code: 2) 
        }
        
        // Fragment
        let hdrSize = 24
        let maxPayload = mtu - hdrSize
        let total = UInt8((data.count + maxPayload - 1) / maxPayload)
        let msgId = UInt64.random(in: .min ... .max)
        
        for i in 0..<Int(total) {
            let start = i * maxPayload
            let end = min(start + maxPayload, data.count)
            let slice = data[start..<end]
            
            var hdr = BridgeHeader(magic: 0xBC77, ver: 1, ttl: 6,
                                   msgId: msgId, gwId: 0, // phone sets 0
                                   fragIdx: UInt8(i), fragTotal: total,
                                   payloadLen: UInt16(slice.count),
                                   hop: 0, crc16: 0)
            var packet = Data(bytes: &hdr, count: hdrSize)
            packet.append(slice)
            // (Optionally compute CRC16 here)
            peripheral.writeValue(packet, for: tx, type: .withoutResponse)
            try await Task.sleep(nanoseconds: 2_000_000) // small pacing
        }
    }
}

// Minimal LRU set
final class LruSet<T: Hashable> {
    private var dict: [T: Int] = [:]
    private var order: [T] = []
    private let cap: Int
    
    init(capacity: Int) { 
        self.cap = capacity 
    }
    
    func contains(_ t: T) -> Bool { 
        dict[t] != nil 
    }
    
    func insert(_ t: T) {
        if dict[t] != nil { return }
        order.append(t)
        dict[t] = 1
        if order.count > cap { 
            let old = order.removeFirst()
            dict.removeValue(forKey: old) 
        }
    }
}
```

### 4.3 Integration into BitChat iOS App

1. Add **GatewayKit** as a local Swift Package to the Xcode project.

2. Introduce a transport multiplexer conforming to the app's existing transport protocol:
   - If the app already has a transport abstraction, implement `GatewayTransport` and add it to the selection logic.
   - Else, create `TransportMux` that merges `DirectBleTransport` and `GatewayTransport` AsyncStreams and picks a write target based on user preference or availability.

#### Transport Mux (example):
```swift
public final class TransportMux: BitChatTransport {
    private let direct: BitChatTransport
    private let gateway: BitChatTransport
    private var stream: AsyncStream<Data>!
    private var cont: AsyncStream<Data>.Continuation!

    public init(direct: BitChatTransport, gateway: BitChatTransport) {
        self.direct = direct
        self.gateway = gateway
        self.stream = AsyncStream<Data> { self.cont = $0 }
        
        Task {
            for await f in direct.frames { 
                self.cont.yield(f) 
            }
        }
        Task {
            for await f in gateway.frames { 
                self.cont.yield(f) 
            }
        }
    }
    
    public var frames: AsyncStream<Data> { stream }
    
    public func start() async { 
        await direct.start()
        await gateway.start() 
    }
    
    public func stop() { 
        direct.stop()
        gateway.stop() 
    }
    
    public func sendOpaque(_ data: Data) async throws {
        // Strategy: prefer direct if peer nearby; else gateway
        do { 
            try await direct.sendOpaque(data) 
        } catch { 
            try await gateway.sendOpaque(data) 
        }
    }
}
```

### 4.4 Presence (optional, app-layer)
- Periodically (e.g., 10s) send small presence frames (username hash, device EID) via `GatewayTransport`.
- Merge presence list with direct BLE neighbors; annotate entries with `via Gateway <gw_id>, hop ~N`.

---

## 5) Message Flow

1. **Phone A** → `GatewayTransport.sendOpaque(BitChatFrameAtoB)`.
2. **Gateway X** receives BLE TX write, wraps in BridgeHdr (or uses phone-provided header), enqueues to Router.
3. **Router on X:** de-dup, TTL--, flood to LoRa and local BLE RX (optional echo suppression).
4. **Gateway Y** LoRa RX: Router de-dup, TTL--, notifies BLE RX to local phones.
5. **Phone B** `GatewayTransport.frames` yields BitChatFrameAtoB to BitChat upper layers (unchanged crypto/E2E).

---

## 6) Acceptance Criteria

- **AC1 Radio Link:** Two Gateways exchange periodic beacon frames over LoRa reliably at SF9/BW125 within legal duty cycle. Loss < 5% at 200 m LOS.
- **AC2 BLE Bridge:** iOS app discovers a Gateway and sends/receives opaque frames ≤ 512 bytes (fragmented) without disconnects for ≥10 minutes.
- **AC3 Loop Safety:** When ≥3 Gateways are powered, frame duplication ≤ 1 extra copy per device; no infinite loops (TTL stops at 0).
- **AC4 De-dup:** Same msg_id not delivered more than once to the iOS app (verified by counters).
- **AC5 Config:** Changing SF/BW/TX over CFG characteristic applies without reboot; persists across reset.
- **AC6 Privacy:** BLE advert rotates EID every 60 seconds (confirmed by scanner logs).

---

## 7) Milestones & Tasks

### M1 — Firmware MVP (BLE↔LoRa passthrough)
- [ ] Implement BridgeHdr, queues, Router, de-dup.
- [ ] NimBLE server with TX/RX/CFG/STATS; advertising with Service UUID + EID placeholder.
- [ ] RadioLib init (EU868 SF9/BW125/CR4/5); send/receive with basic timing.
- [ ] Fragmentation (max LoRa payload ≈ 200 bytes); CRC16.

**Deliverable:** Two Gateways flooding a synthetic payload every 3s; both BLE notify to a phone shows data.

### M2 — iOS GatewayKit
- [ ] GatewayTransport scanning/connect/MTU detect, RX notify, WRITE_NR.
- [ ] Fragment send; basic de-dup by msgId.
- [ ] Public API to BitChat core (BitChatTransport).
- [ ] Minimal SwiftUI debug screen (RSSI, stats, frames/sec).

**Deliverable:** iOS app sends a text payload via Gateway→LoRa→Gateway→back to app (loopback demo).

### M3 — Integration & Stability
- [ ] Integrate TransportMux with existing BitChat message pipeline.
- [ ] Presence beacons (optional).
- [ ] State restoration; background BLE.
- [ ] Stats/CFG UI (read radio params, change SF/BW/TX live).
- [ ] Duty-cycle limiter; CAD/backoff on LoRa; RSSI/SNR logging.

**Deliverable:** Field demo across two Gateways, two iPhones; messages relay with low duplication; UI shows "via Gateway".

### M4 — Hardening
- [ ] Robust reassembly store with timeouts.
- [ ] Bloom filter + LRU for de-dup (memory bounded).
- [ ] EID with HMAC; iOS verifies 6-byte EID stability per 60s window (no PII).
- [ ] OTA pathway placeholder (future).
- [ ] Test suite (firmware: unit tests for CRC, header; iOS: fragmentation, dedup, reconnect).

---

## 8) Testing Plan

### Firmware
- **Unit:** CRC16 vectors; header encode/decode; de-dup ring eviction.
- **Radio soak:** 1hr flood at 0.5 Hz; record RX/TX/stalls.
- **Regulatory:** enforce max duty cycle (EU868 1% typical sub-band) via token bucket per channel.

### iOS
- **MTU variability:** verify on multiple devices; ensure write chunking adheres to `maximumWriteValueLength`.
- **Reconnect:** power off Gateway mid-transfer; confirm resume.
- **Background:** app in background; state restoration reconnects in ≤30s.

---

## 9) Configuration & Defaults

```ini
region=EU868
sf=9
bw=125k
cr=4/5
tx_dbm=14 (or within local legal max)
ttl=6
ble_adv_int_ms=200
```

- **CFG write rules:** allow sf, bw, tx_dbm, ttl changes; persist to NVS on a debounced 1s timer.
- **Safety:** clamp tx_dbm to legal bounds per region.

---

## 10) File/Type Definitions (shared)

### firmware/gateway/include/bridge.h
```cpp
#pragma once
#include <stdint.h>

#define BRIDGE_MAGIC 0xBC77

#pragma pack(push,1)
struct BridgeHdr {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  ttl;
    uint64_t msg_id;
    uint32_t gw_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t payload_len;
    uint16_t hop;
    uint16_t crc16;
};
#pragma pack(pop)

struct Frame {
    BridgeHdr hdr;
    uint16_t  len;
    uint8_t   data[228]; // adjust to match LoRa payload cap
};
```

### ios/BitChat/GatewayKit/Sources/GatewayKit/BridgeHeader.swift
```swift
public struct BridgeHeader {
    public var magic: UInt16
    public var ver: UInt8
    public var ttl: UInt8
    public var msgId: UInt64
    public var gwId: UInt32
    public var fragIdx: UInt8
    public var fragTotal: UInt8
    public var payloadLen: UInt16
    public var hop: UInt16
    public var crc16: UInt16
}
```

---

## 11) Risks & Mitigations

- **Regulatory (LoRa duty cycle / power):** Enforce via token bucket and config clamps.
- **Flooding loops:** TTL + de-dup ring/Bloom, and do not re-echo to the same interface within a short window.
- **BLE MTU variability:** Chunk conservatively; handle CBError.notReady with retry.
- **Privacy/tracking:** Rolling EID; avoid exposing gw_id in adverts; only inside encrypted CFG read if needed.
- **Power/thermal:** SX1262 TX duty cycle limits; stagger transmissions under load.

---

## 12) Developer Notes

- Keep Gateways content-agnostic: do not parse or log plaintext BitChat frames.
- End-to-end crypto remains in BitChat app; Gateways just forward opaque bytes.
- When changing radio params, apply with `radio.standby()` → set → `startReceive()` to avoid lockups.
- Use monotonic timers for expiry; avoid `millis()` wrap issues (handle overflow).

---

## 13) Done-Definition Checklist

- [ ] Two Gateways power on and advertise rolling EID; iOS sees them in scan list.
- [ ] iOS connects, reads CFG, subscribes RX; WRITE_NR works without response errors.
- [ ] Frames traverse BLE→LoRa→BLE end-to-end with fragmentation and CRC verified.
- [ ] No infinite loops with ≥3 Gateways; duplication bounded.
- [ ] Config persists and applies live; STATS increment meaningfully.
- [ ] iOS UI shows "via Gateway <short id> (hop ≤ N)".

---

## 14) Stretch Goals (after P1)

- Multi-channel frequency hopping across allowed sub-bands to spread duty.
- Opportunistic compression (LZ4) for large frames when SNR high.
- Gateway-to-Gateway BLE backhaul (when LoRa congested).
- Mesh RTT estimation and adaptive TTL.
- Simple OTA via BLE CFG (write GZIP block; reboot to OTA partition).