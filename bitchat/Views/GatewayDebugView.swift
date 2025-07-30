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

struct GatewayDebugView: View {
    @EnvironmentObject var viewModel: ChatViewModel
    @State private var isStarted = false
    @State private var lastStats: GatewayStats?
    @State private var lastConfig: GatewayConfig?
    
    private var gatewayTransport: GatewayTransport? {
        viewModel.gatewayTransport
    }
    
    var body: some View {
        NavigationStack {
            List {
                Section("Gateway Status") {
                    HStack {
                        Text("Scanner")
                        Spacer()
                        Text(isStarted ? "Scanning" : "Stopped")
                            .foregroundColor(isStarted ? .blue : .red)
                    }
                    
                    HStack {
                        Text("ESP32 Connection")
                        Spacer()
                        if let gateway = gatewayTransport, gateway.isConnected {
                            Text(gateway.connectedDeviceName ?? "Connected")
                                .foregroundColor(.green)
                        } else {
                            Text("Disconnected")
                                .foregroundColor(.red)
                        }
                    }
                    
                    Button(isStarted ? "Stop Gateway" : "Start Gateway") {
                        Task {
                            guard let gateway = gatewayTransport else {
                                print("GatewayDebugView: Gateway transport not available")
                                return
                            }
                            
                            if isStarted {
                                await MainActor.run {
                                    gateway.stop()
                                    isStarted = false
                                }
                            } else {
                                await gateway.start()
                                await MainActor.run {
                                    isStarted = true
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
                
                Section("Message Test") {
                    Button("Send Test Message") {
                        Task {
                            guard let gateway = gatewayTransport else {
                                print("GatewayDebugView: Gateway transport not available")
                                return
                            }
                            
                            do {
                                let testData = "Hello Gateway!".data(using: .utf8)!
                                print("GatewayDebugView: Attempting to send test message: '\(String(data: testData, encoding: .utf8) ?? "unknown")'")
                                try await gateway.sendOpaque(testData)
                                print("GatewayDebugView: Test message sent successfully!")
                            } catch {
                                print("GatewayDebugView: Failed to send test message: \(error)")
                            }
                        }
                    }
                    .disabled(gatewayTransport?.isConnected != true)
                }
            }
            .navigationTitle("Gateway Debug")
            .task {
                await startMessageListener()
            }
            .onAppear {
                // Sync the UI state with the actual gateway state
                if gatewayTransport != nil {
                    isStarted = true  // Gateway is auto-started in ChatViewModel
                }
            }
        }
    }
    
    private func startMessageListener() async {
        guard let gateway = gatewayTransport else {
            print("GatewayDebugView: Gateway transport not available for message listening")
            return
        }
        
        for await frame in gateway.frames {
            print("Received gateway frame: \(frame.count) bytes")
            if let message = String(data: frame, encoding: .utf8) {
                print("Message content: \(message)")
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