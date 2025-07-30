import Foundation
import CoreBluetooth
import Combine

protocol BitChatTransport {
    func start() async
    func stop()
    func sendOpaque(_ data: Data) async throws
    var frames: AsyncStream<Data> { get }
}

@MainActor
final class GatewayTransport: NSObject, BitChatTransport, ObservableObject {
    static let serviceUUID = CBUUID(string: "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001")
    static let txUUID      = CBUUID(string: "7A1B0001-6B2E-46E8-8D2D-6AA2E5A4F001")  // ESP32 -> iOS
    static let rxUUID      = CBUUID(string: "7A1B0002-6B2E-46E8-8D2D-6AA2E5A4F001")  // iOS -> ESP32
    static let cfgUUID     = CBUUID(string: "7A1B0003-6B2E-46E8-8D2D-6AA2E5A4F001")  // Config/Stats

    private let central = CBCentralManager(delegate: nil, queue: .main, options: [
        CBCentralManagerOptionShowPowerAlertKey: true
    ])

    private var streamCont: AsyncStream<Data>.Continuation?
    private(set) var frames: AsyncStream<Data>
    private var current: GatewayPeripheral?
    private var isScanning = false
    
    @Published var isConnected = false
    @Published var connectedDeviceName: String?

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
            central.scanForPeripherals(withServices: [Self.serviceUUID],
                                     options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            isScanning = true
            print("GatewayTransport: Started scanning for gateways")
        }
    }
    
    func stop() {
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

    func sendOpaque(_ data: Data) async throws {
        guard let p = current else {
            throw GatewayError.notConnected
        }
        try await p.sendBitChatFrame(data)
    }

    // Internal: push frames to the stream
    internal func onOpaqueFrame(_ data: Data) {
        streamCont?.yield(data)
    }
    
    internal func onConnectionLost() {
        current = nil
        isConnected = false
        connectedDeviceName = nil
        
        // Resume scanning
        if central.state == .poweredOn && !isScanning {
            central.scanForPeripherals(withServices: [Self.serviceUUID],
                                     options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            isScanning = true
        }
    }
}

extension GatewayTransport: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        print("GatewayTransport: Bluetooth state: \(central.state.rawValue)")
        
        if central.state == .poweredOn {
            Task {
                await start()
            }
        } else {
            isScanning = false
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                             didDiscover peripheral: CBPeripheral,
                             advertisementData: [String : Any],
                             rssi RSSI: NSNumber) {
        
        // Only connect if we don't have a current connection
        guard current == nil else { return }
        
        print("GatewayTransport: Discovered gateway \(peripheral.identifier), RSSI: \(RSSI)")
        
        // Parse service data for EID and radio params if available
        if let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data],
           let gwData = serviceData[Self.serviceUUID] {
            print("GatewayTransport: Gateway service data: \(gwData.map { String(format: "%02X", $0) }.joined())")
        }
        
        current = GatewayPeripheral(peripheral: peripheral, manager: self)
        central.stopScan()
        isScanning = false
        central.connect(peripheral, options: nil)
    }
    
    func centralManager(_ central: CBCentralManager,
                             didConnect peripheral: CBPeripheral) {
        print("GatewayTransport: Connected to gateway \(peripheral.identifier)")
        
        // Update connection status
        isConnected = true
        connectedDeviceName = peripheral.name ?? "BC-Gateway"
        
        if let current = current, current.peripheral == peripheral {
            peripheral.delegate = current
            peripheral.discoverServices([Self.serviceUUID])
        }
    }
    
    func centralManager(_ central: CBCentralManager,
                             didDisconnectPeripheral peripheral: CBPeripheral,
                             error: Error?) {
        print("GatewayTransport: Disconnected from gateway \(peripheral.identifier), error: \(error?.localizedDescription ?? "none")")
        
        onConnectionLost()
    }
    
    func centralManager(_ central: CBCentralManager,
                             didFailToConnect peripheral: CBPeripheral,
                             error: Error?) {
        print("GatewayTransport: Failed to connect to gateway \(peripheral.identifier), error: \(error?.localizedDescription ?? "unknown")")
        
        onConnectionLost()
    }
}

enum GatewayError: Error {
    case notConnected
    case invalidResponse
    case sendFailed
}