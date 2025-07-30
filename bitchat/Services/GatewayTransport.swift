import Foundation
import CoreBluetooth
import Combine

// Gateway UUID constants
enum GatewayUUIDs {
    static let serviceUUID = CBUUID(string: "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001")
    static let txUUID      = CBUUID(string: "7A1B0001-6B2E-46E8-8D2D-6AA2E5A4F001")  // ESP32 -> iOS
    static let rxUUID      = CBUUID(string: "7A1B0002-6B2E-46E8-8D2D-6AA2E5A4F001")  // iOS -> ESP32
    static let cfgUUID     = CBUUID(string: "7A1B0003-6B2E-46E8-8D2D-6AA2E5A4F001")  // Config/Stats
}

protocol BitChatTransport {
    func start() async
    func stop()
    func sendOpaque(_ data: Data) async throws
    var frames: AsyncStream<Data> { get }
}

// Gateway peer info from status messages
struct GatewayStatusInfo {
    let gatewayId: String
    let gatewayName: String
    let connectedCount: Int
    let nicknames: [String]
}

@MainActor
final class GatewayTransport: NSObject, BitChatTransport, ObservableObject {
    private let central = CBCentralManager(delegate: nil, queue: .main, options: [
        CBCentralManagerOptionShowPowerAlertKey: true
    ])

    private var streamCont: AsyncStream<Data>.Continuation?
    private(set) var frames: AsyncStream<Data>
    private var current: GatewayPeripheral?
    private var isScanning = false
    
    @Published var isConnected = false
    @Published var connectedDeviceName: String?
    @Published var gatewayStatuses: [String: GatewayStatusInfo] = [:] // gatewayId -> status
    
    // Publish gateway status updates
    let statusUpdatePublisher = PassthroughSubject<GatewayStatusInfo, Never>()

    override init() {
        var cont: AsyncStream<Data>.Continuation!
        self.frames = AsyncStream<Data> { c in cont = c }
        self.streamCont = cont
        super.init()
        self.central.delegate = self
    }

    func start() async {
        guard central.state == .poweredOn else {
            print("GatewayTransport: Bluetooth not powered on")
            return
        }
        
        if !isScanning {
            central.scanForPeripherals(withServices: [GatewayUUIDs.serviceUUID],
                                     options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            isScanning = true
            print("GatewayTransport: Started scanning for gateways")
        }
    }
    
    func stop() {
        Task { @MainActor in
            if isScanning {
                central.stopScan()
                isScanning = false
            }
            
            if let p = current {
                central.cancelPeripheralConnection(p.peripheral)
                current = nil
            }
            
            print("GatewayTransport: Stopped")
        }
    }

    func sendOpaque(_ data: Data) async throws {
        guard let p = current else {
            throw GatewayError.notConnected
        }
        
        // Log outgoing message for debug
        print("GatewayTransport: Sending \(data.count) bytes to gateway")
        if let message = String(data: data, encoding: .utf8) {
            print("GatewayTransport: Message content: \(message)")
        }
        
        try await p.sendBitChatFrame(data)
    }

    // Internal: push frames to the stream
    internal func onOpaqueFrame(_ data: Data) {
        // Always yield the frame for debug view
        streamCont?.yield(data)
        
        // Also check if this is a status message
        if let statusInfo = parseStatusMessage(data) {
            // Update our status dictionary
            gatewayStatuses[statusInfo.gatewayId] = statusInfo
            // Notify subscribers
            statusUpdatePublisher.send(statusInfo)
            print("GatewayTransport: Received status from \(statusInfo.gatewayName): \(statusInfo.connectedCount) devices")
        }
    }
    
    // Parse status messages from gateways
    private func parseStatusMessage(_ data: Data) -> GatewayStatusInfo? {
        guard let message = String(data: data, encoding: .utf8),
              message.hasPrefix("STATUS|") else {
            return nil
        }
        
        let parts = message.split(separator: "|")
        guard parts.count >= 4 else { return nil }
        
        let gatewayId = String(parts[1])
        let gatewayName = String(parts[2])
        let connectedCount = Int(parts[3]) ?? 0
        
        var nicknames: [String] = []
        if parts.count > 4 {
            nicknames = String(parts[4]).split(separator: ",").map { String($0) }
        }
        
        return GatewayStatusInfo(
            gatewayId: gatewayId,
            gatewayName: gatewayName,
            connectedCount: connectedCount,
            nicknames: nicknames
        )
    }
    
    internal func onConnectionLost() {
        current = nil
        isConnected = false
        connectedDeviceName = nil
        
        // Send disconnection notification
        NotificationCenter.default.post(
            name: Notification.Name("GatewayDisconnected"),
            object: self
        )
        
        // Resume scanning
        if central.state == .poweredOn && !isScanning {
            central.scanForPeripherals(withServices: [GatewayUUIDs.serviceUUID],
                                     options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            isScanning = true
        }
    }
}

extension GatewayTransport: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            print("GatewayTransport: Bluetooth state: \(central.state.rawValue)")
            
            if central.state == .poweredOn && !isScanning {
                central.scanForPeripherals(withServices: [GatewayUUIDs.serviceUUID],
                                         options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
                isScanning = true
                print("GatewayTransport: Started scanning for gateways")
            }
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String : Any],
                       rssi RSSI: NSNumber) {
        Task { @MainActor in
            guard current == nil else { return }
            
            print("GatewayTransport: Found gateway: \(peripheral.name ?? "Unknown") RSSI: \(RSSI)")
            
            // Check service data for gateway info
            if let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data],
               let data = serviceData[GatewayUUIDs.serviceUUID] {
                print("GatewayTransport: Service data: \(data.hexEncodedString())")
            }
            
            current = GatewayPeripheral(peripheral: peripheral, manager: self)
            central.stopScan()
            isScanning = false
            central.connect(peripheral, options: nil)
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                       didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            print("GatewayTransport: Connected to gateway \(peripheral.identifier)")
            
            isConnected = true
            connectedDeviceName = peripheral.name
            
            // Send notification
            NotificationCenter.default.post(
                name: Notification.Name("GatewayConnected"),
                object: self,
                userInfo: ["deviceName": connectedDeviceName ?? "Unknown"]
            )
            
            if let current = current, current.peripheral == peripheral {
                peripheral.delegate = current
                peripheral.discoverServices([GatewayUUIDs.serviceUUID])
            }
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                             didDisconnectPeripheral peripheral: CBPeripheral,
                             error: Error?) {
        Task { @MainActor in
            print("GatewayTransport: Disconnected from gateway \(peripheral.identifier), error: \(error?.localizedDescription ?? "none")")
            
            onConnectionLost()
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                             didFailToConnect peripheral: CBPeripheral,
                             error: Error?) {
        Task { @MainActor in
            print("GatewayTransport: Failed to connect to gateway \(peripheral.identifier), error: \(error?.localizedDescription ?? "unknown")")
            
            onConnectionLost()
        }
    }
}

enum GatewayError: Error {
    case notConnected
    case invalidResponse
    case sendFailed
}