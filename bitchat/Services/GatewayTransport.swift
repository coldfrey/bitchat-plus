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
    nonisolated let frames: AsyncStream<Data>
    private var current: GatewayPeripheral?
    private var isScanning = false
    
    @Published var isConnected = false
    @Published var connectedDeviceName: String?
    @Published var gatewayStatuses: [String: GatewayStatusInfo] = [:] // gatewayId -> status
    
    // Publish gateway status updates
    let statusUpdatePublisher = PassthroughSubject<GatewayStatusInfo, Never>()
    
    // Publish all frames for multiple consumers
    let framePublisher = PassthroughSubject<Data, Never>()

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
    
    nonisolated func stop() {
        Task { @MainActor in
            _stop()
        }
    }
    
    // MARK: - Private Methods
    
    private func startScanning() {
        guard !isScanning && central.state == .poweredOn else { return }
        
        central.scanForPeripherals(
            withServices: [GatewayUUIDs.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
        isScanning = true
        print("GatewayTransport: Started scanning for gateways")
    }
    
    private func stopScanning() {
        guard isScanning else { return }
        
        central.stopScan()
        isScanning = false
        print("GatewayTransport: Stopped scanning")
    }
    
    private func connect(to peripheral: CBPeripheral) {
        stopScanning()
        current = GatewayPeripheral(peripheral: peripheral, manager: self)
        central.connect(peripheral, options: nil)
    }
    
    private func disconnect() {
        if let peripheral = current?.peripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        current = nil
    }
    
    private func _stop() {
        stopScanning()
        disconnect()
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
        // Always yield the frame for debug view (legacy AsyncStream)
        streamCont?.yield(data)
        
        // Also publish for multiple consumers
        framePublisher.send(data)
        
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
        // First, try to convert to string
        guard let message = String(data: data, encoding: .utf8) else {
            // If direct UTF8 conversion fails, try to clean the data
            var cleanedData = Data()
            for byte in data {
                // Only include printable ASCII characters and newlines
                if (byte >= 32 && byte <= 126) || byte == 10 || byte == 13 {
                    cleanedData.append(byte)
                }
            }
            
            guard let cleanedMessage = String(data: cleanedData, encoding: .utf8) else {
                print("GatewayTransport: Failed to parse status message even after cleaning")
                return nil
            }
            
            return parseStatusString(cleanedMessage)
        }
        
        return parseStatusString(message)
    }
    
    private func parseStatusString(_ message: String) -> GatewayStatusInfo? {
        // Trim whitespace and control characters
        let trimmedMessage = message.trimmingCharacters(in: .whitespacesAndNewlines)
        
        // Also remove any non-printable characters at the start
        let cleanedMessage = trimmedMessage.drop(while: { char in
            let scalar = char.unicodeScalars.first!
            return scalar.value < 32 || scalar.value > 126
        })
        
        let finalMessage = String(cleanedMessage)
        
        print("GatewayTransport: parseStatusMessage - Final cleaned message: '\(finalMessage)'")
        
        guard finalMessage.hasPrefix("STATUS|") else {
            print("GatewayTransport: parseStatusMessage - Does not start with STATUS|")
            return nil
        }
        
        let parts = finalMessage.split(separator: "|")
        guard parts.count >= 3 else { 
            print("GatewayTransport: parseStatusMessage - Not enough parts: \(parts.count)")
            return nil 
        }
        
        let gatewayId = String(parts[1])
        let gatewayName = String(parts[2])
        
        // Device count is optional (might be missing if truncated)
        let connectedCount = parts.count > 3 ? Int(String(parts[3])) ?? 0 : 0
        
        // Parse connected nicknames if present
        var nicknames: [String] = []
        if parts.count > 4 {
            nicknames = String(parts[4]).split(separator: ",").map { String($0) }
        }
        
        print("GatewayTransport: Parsed status - Gateway: \(gatewayName), ID: \(gatewayId), Count: \(connectedCount)")
        
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
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            _centralManagerDidUpdateState(central)
        }
    }
    
    @MainActor
    private func _centralManagerDidUpdateState(_ central: CBCentralManager) {
        print("GatewayTransport: Bluetooth state: \(central.state.rawValue)")
        
        switch central.state {
        case .poweredOn:
            startScanning()
        case .poweredOff:
            print("GatewayTransport: Bluetooth powered off")
            stopScanning()
            disconnect()
        default:
            print("GatewayTransport: Bluetooth not powered on")
        }
    }
    
    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        Task { @MainActor in
            _centralManager(central, didDiscover: peripheral, advertisementData: advertisementData, rssi: RSSI)
        }
    }
    
    @MainActor
    private func _centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        guard let name = peripheral.name, name.hasPrefix("BC-") else { return }
        
        print("GatewayTransport: Found gateway: \(name) RSSI: \(RSSI)")
        
        // Log service data if available
        if let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
            for (_, data) in serviceData {
                print("GatewayTransport: Service data: \(data.hexEncodedString())")
            }
        }
        
        // Auto-connect to first gateway found
        if current == nil {
            connect(to: peripheral)
        }
    }
    
    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            _centralManager(central, didConnect: peripheral)
        }
    }
    
    @MainActor
    private func _centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("GatewayTransport: Connected to gateway \(peripheral.identifier)")
        isConnected = true
        connectedDeviceName = peripheral.name
        
        // Discover services on the peripheral
        if let current = current, current.peripheral == peripheral {
            peripheral.delegate = current
            peripheral.discoverServices([GatewayUUIDs.serviceUUID])
        }
    }
    
    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            _centralManager(central, didFailToConnect: peripheral, error: error)
        }
    }
    
    @MainActor
    private func _centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        print("GatewayTransport: Failed to connect: \(error?.localizedDescription ?? "unknown")")
        isConnected = false
        connectedDeviceName = nil
        current = nil
        
        // Resume scanning
        startScanning()
    }
    
    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            _centralManager(central, didDisconnectPeripheral: peripheral, error: error)
        }
    }
    
    @MainActor
    private func _centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("GatewayTransport: Disconnected from gateway: \(error?.localizedDescription ?? "user initiated")")
        isConnected = false
        connectedDeviceName = nil
        current = nil
        
        // Resume scanning
        startScanning()
    }
}

enum GatewayError: Error {
    case notConnected
    case invalidResponse
    case sendFailed
}