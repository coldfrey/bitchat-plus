import Foundation
import CoreBluetooth

@MainActor
final class GatewayPeripheral: NSObject, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    unowned let manager: GatewayTransport

    private var chTX: CBCharacteristic?
    private var chRX: CBCharacteristic?
    private var chCFG: CBCharacteristic?
    
    private var mtu: Int = 180
    private var dedup = LruSet<UInt64>(capacity: 2048)
    private var isReady = false

    init(peripheral: CBPeripheral, manager: GatewayTransport) {
        self.peripheral = peripheral
        self.manager = manager
        super.init()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil else {
            print("GatewayPeripheral: Service discovery failed: \(error!)")
            return
        }
        
        for service in peripheral.services ?? [] where service.uuid == GatewayTransport.serviceUUID {
            peripheral.discoverCharacteristics([
                GatewayTransport.txUUID,
                GatewayTransport.rxUUID,
                GatewayTransport.cfgUUID
            ], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverCharacteristicsFor service: CBService,
                   error: Error?) {
        guard error == nil else {
            print("GatewayPeripheral: Characteristic discovery failed: \(error!)")
            return
        }
        
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case GatewayTransport.txUUID:
                chTX = characteristic
                print("GatewayPeripheral: Found TX characteristic")
                
            case GatewayTransport.rxUUID:
                chRX = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                print("GatewayPeripheral: Found RX characteristic, enabled notifications")
                
            case GatewayTransport.cfgUUID:
                chCFG = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
                peripheral.readValue(for: characteristic)
                print("GatewayPeripheral: Found CFG characteristic, enabled notifications and reading initial value")
                
            default:
                break
            }
        }
        
        // Determine MTU
        mtu = peripheral.maximumWriteValueLength(for: .withoutResponse)
        print("GatewayPeripheral: MTU = \(mtu)")
        
        // Mark as ready if we have essential characteristics
        if chTX != nil && chRX != nil {
            isReady = true
            print("GatewayPeripheral: Gateway ready for communication")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didUpdateValueFor characteristic: CBCharacteristic,
                   error: Error?) {
        guard error == nil else {
            print("GatewayPeripheral: Read error: \(error!)")
            return
        }
        
        guard let value = characteristic.value else {
            print("GatewayPeripheral: No value for characteristic \(characteristic.uuid)")
            return
        }
        
        // Handle different characteristic types
        switch characteristic.uuid {
        case GatewayTransport.rxUUID:
            handleRxData(value)
        case GatewayTransport.cfgUUID:
            handleConfigData(value)
        default:
            print("GatewayPeripheral: Unknown characteristic updated: \(characteristic.uuid)")
        }
    }
    
    private func handleRxData(_ value: Data) {
        guard value.count >= 24 else {
            print("GatewayPeripheral: RX data too short: \(value.count) bytes")
            return
        }
        
        // Parse bridge header
        guard let header = BridgeHeader.fromData(value) else {
            print("GatewayPeripheral: Invalid bridge header")
            return
        }
        
        // Validate magic
        guard header.magic == 0xBC77 else {
            print("GatewayPeripheral: Invalid magic: 0x\(String(header.magic, radix: 16))")
            return
        }
        
        // Check for duplicates
        if dedup.contains(header.msgId) {
            print("GatewayPeripheral: Duplicate message: \(header.msgId)")
            return
        }
        
        dedup.insert(header.msgId)
        
        // Extract payload
        let payload = value.dropFirst(24)
        
        print("GatewayPeripheral: Received frame: msgId=\(header.msgId), ttl=\(header.ttl), hop=\(header.hop), len=\(payload.count)")
        
        // Forward to manager
        manager.onOpaqueFrame(Data(payload))
    }
    
    private func handleConfigData(_ value: Data) {
        if let configString = String(data: value, encoding: .utf8) {
            print("GatewayPeripheral: Config/Stats update: \(configString)")
        } else {
            print("GatewayPeripheral: Config/Stats binary data: \(value.count) bytes")
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            print("GatewayPeripheral: Write error: \(error)")
        }
    }

    func sendBitChatFrame(_ data: Data) async throws {
        guard isReady, let rx = chRX else {
            throw GatewayError.notConnected
        }
        
        // Fragment the data
        let headerSize = 24
        let maxPayload = mtu - headerSize
        let totalFragments = UInt8((data.count + maxPayload - 1) / maxPayload)
        let msgId = UInt64.random(in: .min ... .max)
        
        print("GatewayPeripheral: Sending frame: len=\(data.count), fragments=\(totalFragments), msgId=\(msgId)")
        
        for i in 0..<Int(totalFragments) {
            let start = i * maxPayload
            let end = min(start + maxPayload, data.count)
            let slice = data[start..<end]
            
            let header = BridgeHeader(
                ttl: 6,
                msgId: msgId,
                gwId: 0, // Phone sets 0
                fragIdx: UInt8(i),
                fragTotal: totalFragments,
                payloadLen: UInt16(slice.count),
                hop: 0
            )
            
            var packet = header.toData()
            packet.append(slice)
            
            peripheral.writeValue(packet, for: rx, type: .withoutResponse)
            
            // Small pacing delay for multiple fragments
            if totalFragments > 1 {
                try await Task.sleep(nanoseconds: 2_000_000) // 2ms
            }
        }
        
        print("GatewayPeripheral: Frame sent successfully")
    }
}

// Minimal LRU set for deduplication
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