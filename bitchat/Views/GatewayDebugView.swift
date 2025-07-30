//
// GatewayDebugView.swift
// bitchat
//
// This is free and unencumbered software released into the public domain.
// For more information, see <https://unlicense.org>
//

///
/// # GatewayDebugView
///
/// Debug interface for Gateway functionality showing connection status,
/// statistics, and configuration options.
///
/// This view provides developers and advanced users with visibility
/// into the Gateway transport system for debugging and monitoring.
///

import SwiftUI

// Message tracking for debug view
struct GatewayMessage: Identifiable {
    let id = UUID()
    let timestamp: Date
    let direction: MessageDirection
    let content: String
    let size: Int
    let isStatus: Bool
    
    enum MessageDirection {
        case sent
        case received
        case failed
    }
}

struct GatewayDebugView: View {
    @EnvironmentObject var viewModel: ChatViewModel
    @State private var isStarted = false
    @State private var lastStats: GatewayStats?
    @State private var lastConfig: GatewayConfig?
    @State private var messages: [GatewayMessage] = []
    @State private var autoScroll = true
    @State private var showOnlyStatus = false
    @State private var connectionStartTime: Date?
    @State private var messageListener: Task<Void, Never>?
    
    private var gatewayTransport: GatewayTransport? {
        viewModel.gatewayTransport
    }
    
    private var connectionDuration: String {
        guard let startTime = connectionStartTime else { return "Not connected" }
        let duration = Date().timeIntervalSince(startTime)
        let minutes = Int(duration) / 60
        let seconds = Int(duration) % 60
        return String(format: "%d:%02d", minutes, seconds)
    }
    
    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                // Connection Header
                connectionHeader
                    .padding()
                    .background(Color(UIColor.systemBackground))
                    .shadow(radius: 2)
                
                List {
                    // Message Log Section
                    Section("Message Log") {
                        messageControls
                        
                        if messages.isEmpty {
                            Text("No messages yet...")
                                .foregroundColor(.secondary)
                                .italic()
                                .frame(maxWidth: .infinity)
                                .padding()
                        } else {
                            ScrollViewReader { proxy in
                                ForEach(filteredMessages) { message in
                                    messageRow(message)
                                        .id(message.id)
                                }
                                .onChange(of: messages.count) { _ in
                                    if autoScroll, let lastMessage = messages.last {
                                        withAnimation {
                                            proxy.scrollTo(lastMessage.id, anchor: .bottom)
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    if let stats = lastStats {
                        Section("Statistics") {
                            StatRow(label: "Uptime", value: "\(stats.uptime)s")
                            StatRow(label: "LoRa RX", value: "\(stats.loraRx)")
                            StatRow(label: "LoRa TX", value: "\(stats.loraTx)")
                            StatRow(label: "BLE RX", value: "\(stats.bleRx)")
                            StatRow(label: "BLE TX", value: "\(stats.bleTx)")
                            StatRow(label: "Dedup Hits", value: "\(stats.dedupHits)")
                            StatRow(label: "CRC Errors", value: "\(stats.crcErrors)")
                            StatRow(label: "Duty Block", value: "\(stats.dutyBlockMs)ms")
                        }
                    }
                    
                    if let config = lastConfig {
                        Section("Configuration") {
                            StatRow(label: "Gateway ID", value: String(format: "%08X", config.gwId))
                            StatRow(label: "Region", value: regionName(config.region))
                            StatRow(label: "Spreading Factor", value: "SF\(config.sf)")
                            StatRow(label: "Bandwidth", value: bandwidthName(config.bw))
                            StatRow(label: "TX Power", value: "\(config.txDbm) dBm")
                            StatRow(label: "TTL", value: "\(config.ttl)")
                            StatRow(label: "BLE Adv Interval", value: "\(config.bleAdvIntMs) ms")
                        }
                    }
                    
                    if let gatewayTransport = gatewayTransport,
                       !gatewayTransport.gatewayStatuses.isEmpty {
                        Section("Network Gateways") {
                            ForEach(Array(gatewayTransport.gatewayStatuses.values), id: \.gatewayId) { status in
                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text(status.gatewayName)
                                            .font(.headline)
                                        Spacer()
                                        Text("\(status.connectedCount) devices")
                                            .font(.caption)
                                            .foregroundColor(.secondary)
                                    }
                                    
                                    if !status.nicknames.isEmpty {
                                        Text("Connected: \(status.nicknames.joined(separator: ", "))")
                                            .font(.caption)
                                            .foregroundColor(.blue)
                                    }
                                }
                                .padding(.vertical, 4)
                            }
                        }
                    }
                    
                    Section("Actions") {
                        Button("Send Test Message") {
                            sendTestMessage()
                        }
                        .disabled(gatewayTransport?.isConnected != true)
                        
                        Button("Clear Message Log") {
                            messages.removeAll()
                        }
                        .foregroundColor(.red)
                        
                        if gatewayTransport?.isConnected == true {
                            Button("Disconnect Gateway") {
                                disconnectGateway()
                            }
                            .foregroundColor(.orange)
                        } else {
                            Button("Reconnect Gateway") {
                                reconnectGateway()
                            }
                            .foregroundColor(.blue)
                        }
                    }
                }
            }
            .navigationTitle("Gateway Debug")
            .navigationBarTitleDisplayMode(.inline)
            .task {
                await startMessageListener()
            }
            .onAppear {
                setupGatewayObservers()
            }
            .onDisappear {
                messageListener?.cancel()
            }
        }
    }
    
    // MARK: - View Components
    
    private var connectionHeader: some View {
        VStack(spacing: 8) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Gateway Status")
                        .font(.headline)
                    
                    if let gateway = gatewayTransport, gateway.isConnected {
                        Text(gateway.connectedDeviceName ?? "Connected")
                            .font(.subheadline)
                            .foregroundColor(.green)
                    } else {
                        Text("Disconnected")
                            .font(.subheadline)
                            .foregroundColor(.red)
                    }
                }
                
                Spacer()
                
                VStack(alignment: .trailing, spacing: 4) {
                    if gatewayTransport?.isConnected == true {
                        Image(systemName: "antenna.radiowaves.left.and.right")
                            .foregroundColor(.green)
                            .font(.title2)
                    } else {
                        Image(systemName: "antenna.radiowaves.left.and.right.slash")
                            .foregroundColor(.red)
                            .font(.title2)
                    }
                    
                    Text(connectionDuration)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            
            // Gateway peer status
            if !viewModel.gatewayPeers.isEmpty {
                Divider()
                VStack(alignment: .leading, spacing: 4) {
                    Text("Remote Peers via Gateways:")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    
                    ForEach(viewModel.gatewayPeers) { peer in
                        HStack {
                            Image(systemName: "person.fill")
                                .font(.caption)
                            Text("\(peer.nickname)")
                                .font(.caption.monospaced())
                            Text("[\(peer.gatewayName)]")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
    
    private var messageControls: some View {
        HStack {
            Toggle("Auto-scroll", isOn: $autoScroll)
                .font(.caption)
            
            Spacer()
            
            Toggle("Status only", isOn: $showOnlyStatus)
                .font(.caption)
        }
        .padding(.horizontal, 4)
    }
    
    private var filteredMessages: [GatewayMessage] {
        if showOnlyStatus {
            return messages.filter { $0.isStatus }
        }
        return messages
    }
    
    private func messageRow(_ message: GatewayMessage) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: iconForMessage(message))
                    .foregroundColor(colorForMessage(message))
                    .font(.caption)
                
                Text(formatTimestamp(message.timestamp))
                    .font(.caption.monospaced())
                    .foregroundColor(.secondary)
                
                Text(message.direction == .sent ? "→" : "←")
                    .font(.caption.monospaced())
                    .foregroundColor(colorForMessage(message))
                
                Text("\(message.size) bytes")
                    .font(.caption)
                    .foregroundColor(.secondary)
                
                if message.isStatus {
                    Text("STATUS")
                        .font(.caption)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Color.blue.opacity(0.2))
                        .cornerRadius(4)
                    
                    // Parse and show status details
                    if let statusDetails = parseStatusDetails(message.content) {
                        Text("[\(statusDetails)]")
                            .font(.caption)
                            .foregroundColor(.blue)
                    }
                }
                
                Spacer()
            }
            
            Text(message.content)
                .font(.caption.monospaced())
                .foregroundColor(message.direction == .failed ? .red : .primary)
                .lineLimit(3)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.vertical, 4)
    }
    
    // MARK: - Helper Functions
    
    private func iconForMessage(_ message: GatewayMessage) -> String {
        switch message.direction {
        case .sent:
            return "arrow.up.circle"
        case .received:
            return "arrow.down.circle"
        case .failed:
            return "exclamationmark.circle"
        }
    }
    
    private func colorForMessage(_ message: GatewayMessage) -> Color {
        switch message.direction {
        case .sent:
            return .blue
        case .received:
            return .green
        case .failed:
            return .red
        }
    }
    
    private func formatTimestamp(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter.string(from: date)
    }
    
    private func parseStatusDetails(_ content: String) -> String? {
        // Parse STATUS|gateway_id|gateway_name|connected_count|nicknames
        let parts = content.split(separator: "|")
        guard parts.count >= 4, parts[0] == "STATUS" else { return nil }
        
        let gatewayId = String(parts[1])
        let gatewayName = String(parts[2])
        let connectedCount = String(parts[3])
        
        var details = "\(gatewayName) (ID: \(gatewayId)): \(connectedCount) devices"
        
        if parts.count > 4 && !parts[4].isEmpty {
            let nicknames = String(parts[4])
            details += " - Users: \(nicknames)"
        }
        
        return details
    }
    
    // MARK: - Gateway Operations
    
    private func setupGatewayObservers() {
        // Sync the UI state with the actual gateway state
        if let gateway = gatewayTransport {
            isStarted = true  // Gateway is auto-started in ChatViewModel
            
            if gateway.isConnected {
                connectionStartTime = Date()
            }
        }
    }
    
    private func startMessageListener() async {
        guard let gateway = gatewayTransport else {
            print("GatewayDebugView: Gateway transport not available for message listening")
            return
        }
        
        // Cancel any existing listener
        messageListener?.cancel()
        
        messageListener = Task {
            for await frame in gateway.frames {
                guard !Task.isCancelled else { break }
                
                await MainActor.run {
                    let content = String(data: frame, encoding: .utf8) ?? frame.hexEncodedString()
                    let isStatus = content.hasPrefix("STATUS|")
                    
                    let message = GatewayMessage(
                        timestamp: Date(),
                        direction: .received,
                        content: content,
                        size: frame.count,
                        isStatus: isStatus
                    )
                    
                    messages.append(message)
                    
                    // Keep only last 100 messages
                    if messages.count > 100 {
                        messages.removeFirst(messages.count - 100)
                    }
                    
                    print("GatewayDebugView: Received frame: \(frame.count) bytes - \(content)")
                }
            }
        }
    }
    
    private func sendTestMessage() {
        Task {
            guard let gateway = gatewayTransport else {
                print("GatewayDebugView: Gateway transport not available")
                return
            }
            
            let testContent = "Test from iOS @ \(Date().timeIntervalSince1970)"
            let testData = testContent.data(using: .utf8)!
            
            do {
                // Log the outgoing message
                await MainActor.run {
                    let message = GatewayMessage(
                        timestamp: Date(),
                        direction: .sent,
                        content: testContent,
                        size: testData.count,
                        isStatus: false
                    )
                    messages.append(message)
                }
                
                try await gateway.sendOpaque(testData)
                print("GatewayDebugView: Test message sent successfully!")
            } catch {
                print("GatewayDebugView: Failed to send test message: \(error)")
                
                // Log the failure
                await MainActor.run {
                    let message = GatewayMessage(
                        timestamp: Date(),
                        direction: .failed,
                        content: "Failed: \(error.localizedDescription)",
                        size: 0,
                        isStatus: false
                    )
                    messages.append(message)
                }
            }
        }
    }
    
    private func disconnectGateway() {
        gatewayTransport?.stop()
        connectionStartTime = nil
        messageListener?.cancel()
        
        // Log disconnection
        let message = GatewayMessage(
            timestamp: Date(),
            direction: .failed,
            content: "Gateway disconnected by user",
            size: 0,
            isStatus: false
        )
        messages.append(message)
    }
    
    private func reconnectGateway() {
        Task {
            // Log reconnection attempt
            await MainActor.run {
                let message = GatewayMessage(
                    timestamp: Date(),
                    direction: .sent,
                    content: "Attempting to reconnect...",
                    size: 0,
                    isStatus: false
                )
                messages.append(message)
            }
            
            await gatewayTransport?.start()
            
            // Check if connected
            if gatewayTransport?.isConnected == true {
                await MainActor.run {
                    connectionStartTime = Date()
                    let message = GatewayMessage(
                        timestamp: Date(),
                        direction: .received,
                        content: "Gateway reconnected successfully",
                        size: 0,
                        isStatus: false
                    )
                    messages.append(message)
                }
                
                // Restart message listener
                await startMessageListener()
            }
        }
    }
    
    private func regionName(_ region: UInt8) -> String {
        switch region {
        case 0: return "EU868"
        case 1: return "US915"
        case 2: return "AS923"
        default: return "Unknown"
        }
    }
    
    private func bandwidthName(_ bw: UInt8) -> String {
        switch bw {
        case 0: return "125 kHz"
        case 1: return "250 kHz"
        case 2: return "500 kHz"
        default: return "Unknown"
        }
    }
}

struct StatRow: View {
    let label: String
    let value: String
    
    var body: some View {
        HStack {
            Text(label)
            Spacer()
            Text(value)
                .foregroundColor(.secondary)
        }
    }
}

#Preview {
    GatewayDebugView()
}