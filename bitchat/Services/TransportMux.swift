//
// TransportMux.swift
// bitchat
//
// This is free and unencumbered software released into the public domain.
// For more information, see <https://unlicense.org>
//

///
/// # TransportMux
///
/// Transport multiplexer that combines direct peer-to-peer Bluetooth mesh
/// with Gateway-based LoRa transport for extended range communication.
///
/// ## Overview
/// TransportMux provides a unified interface that combines:
/// - **Direct Transport**: Native BitChat peer-to-peer BLE mesh
/// - **Gateway Transport**: Range extension via LoRa gateways
///
/// Messages are automatically routed based on availability and preference.
/// The system maintains compatibility with existing BitChat protocol while
/// extending reach through gateway infrastructure.
///
/// ## Architecture
/// - Merges message streams from both transports
/// - Implements intelligent routing (prefer direct, fallback to gateway)
/// - Maintains transport-agnostic interface for upper layers
/// - Handles transport-specific features (stats, configuration)
///

import Foundation
import Combine

/// Protocol for BitChat transport abstraction
protocol BitChatTransportProtocol {
    var isAvailable: Bool { get }
    func sendMessage(_ data: Data) async throws
    var messageStream: AnyPublisher<Data, Never> { get }
    func start() async
    func stop()
}

/// Direct BLE mesh transport adapter
class DirectTransportAdapter: BitChatTransportProtocol {
    private let meshService: BluetoothMeshService
    private let messageSubject = PassthroughSubject<Data, Never>()
    
    var isAvailable: Bool {
        // Check if mesh service has any connected peers
        !meshService.getPeerNicknames().isEmpty
    }
    
    var messageStream: AnyPublisher<Data, Never> {
        messageSubject.eraseToAnyPublisher()
    }
    
    init(meshService: BluetoothMeshService) {
        self.meshService = meshService
        // Note: In a real implementation, we'd need to adapt the mesh service
        // to provide raw data frames and send raw data frames
    }
    
    func sendMessage(_ data: Data) async throws {
        // Convert raw data to BitChat protocol message and send via mesh service
        // This would need integration with the existing mesh service API
        throw NSError(domain: "DirectTransport", code: 1, userInfo: [NSLocalizedDescriptionKey: "Not yet implemented"])
    }
    
    func start() async {
        // Mesh service is managed externally
    }
    
    func stop() {
        // Mesh service is managed externally
    }
}

/// Gateway transport adapter
class GatewayTransportAdapter: BitChatTransportProtocol {
    private let gatewayTransport: GatewayTransport
    private var messageTask: Task<Void, Never>?
    private let messageSubject = PassthroughSubject<Data, Never>()
    
    var isAvailable: Bool {
        // Could check if gateway is connected, for now assume available
        true
    }
    
    var messageStream: AnyPublisher<Data, Never> {
        messageSubject.eraseToAnyPublisher()
    }
    
    init(gatewayTransport: GatewayTransport) {
        self.gatewayTransport = gatewayTransport
        startMessageStream()
    }
    
    private func startMessageStream() {
        messageTask = Task { @MainActor in
            for await frame in gatewayTransport.frames {
                messageSubject.send(frame)
            }
        }
    }
    
    func sendMessage(_ data: Data) async throws {
        try await gatewayTransport.sendOpaque(data)
    }
    
    func start() async {
        await gatewayTransport.start()
    }
    
    func stop() {
        Task { @MainActor in
            gatewayTransport.stop()
        }
        messageTask?.cancel()
    }
    
    deinit {
        messageTask?.cancel()
    }
}

/// Transport multiplexer that combines direct and gateway transports
@MainActor
class TransportMux: ObservableObject {
    private let directTransport: DirectTransportAdapter
    private let gatewayTransport: GatewayTransportAdapter
    private var messageCancellables = Set<AnyCancellable>()
    
    // Published properties for UI
    @Published var isDirectConnected = false
    @Published var isGatewayConnected = false
    @Published var preferredTransport: TransportType = .auto
    
    // Message stream combining both transports
    private let messageSubject = PassthroughSubject<Data, Never>()
    var messageStream: AnyPublisher<Data, Never> {
        messageSubject.eraseToAnyPublisher()
    }
    
    enum TransportType {
        case direct
        case gateway
        case auto
    }
    
    init(meshService: BluetoothMeshService) {
        self.directTransport = DirectTransportAdapter(meshService: meshService)
        self.gatewayTransport = GatewayTransportAdapter(gatewayTransport: GatewayTransport())
        
        setupMessageStreams()
    }
    
    private func setupMessageStreams() {
        // Merge message streams from both transports
        directTransport.messageStream
            .sink { [weak self] data in
                self?.messageSubject.send(data)
            }
            .store(in: &messageCancellables)
        
        gatewayTransport.messageStream
            .sink { [weak self] data in
                self?.messageSubject.send(data)
            }
            .store(in: &messageCancellables)
    }
    
    func start() async {
        await directTransport.start()
        await gatewayTransport.start()
    }
    
    func stop() {
        directTransport.stop()
        gatewayTransport.stop()
    }
    
    func sendMessage(_ data: Data) async throws {
        switch preferredTransport {
        case .direct:
            try await sendViaDirect(data)
        case .gateway:
            try await sendViaGateway(data)
        case .auto:
            try await sendViaAuto(data)
        }
    }
    
    private func sendViaDirect(_ data: Data) async throws {
        guard directTransport.isAvailable else {
            throw TransportError.directNotAvailable
        }
        try await directTransport.sendMessage(data)
    }
    
    private func sendViaGateway(_ data: Data) async throws {
        guard gatewayTransport.isAvailable else {
            throw TransportError.gatewayNotAvailable
        }
        try await gatewayTransport.sendMessage(data)
    }
    
    private func sendViaAuto(_ data: Data) async throws {
        // Strategy: prefer direct if available, fallback to gateway
        if directTransport.isAvailable {
            do {
                try await directTransport.sendMessage(data)
                return
            } catch {
                print("TransportMux: Direct send failed, trying gateway: \(error)")
            }
        }
        
        if gatewayTransport.isAvailable {
            try await gatewayTransport.sendMessage(data)
        } else {
            throw TransportError.noTransportAvailable
        }
    }
}

public enum TransportError: Error {
    case directNotAvailable
    case gatewayNotAvailable
    case noTransportAvailable
}