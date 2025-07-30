//
// ChatViewModel.swift
// bitchat
//
// This is free and unencumbered software released into the public domain.
// For more information, see <https://unlicense.org>
//

///
/// # ChatViewModel
///
/// The central business logic and state management component for BitChat.
/// Coordinates between the UI layer and the networking/encryption services.
///
/// ## Overview
/// ChatViewModel implements the MVVM pattern, serving as the binding layer between
/// SwiftUI views and the underlying BitChat services. It manages:
/// - Message state and delivery
/// - Peer connections and presence
/// - Private chat sessions
/// - Command processing
/// - UI state like autocomplete and notifications
///
/// ## Architecture
/// The ViewModel acts as:
/// - **BitchatDelegate**: Receives messages and events from BluetoothMeshService
/// - **State Manager**: Maintains all UI-relevant state with @Published properties
/// - **Command Processor**: Handles IRC-style commands (/msg, /who, etc.)
/// - **Message Router**: Directs messages to appropriate chats (public/private)
///
/// ## Key Features
///
/// ### Message Management
/// - Batches incoming messages for performance (100ms window)
/// - Maintains separate public and private message queues
/// - Limits message history to prevent memory issues (1337 messages)
/// - Tracks delivery and read receipts
///
/// ### Privacy Features
/// - Ephemeral by design - no persistent message storage
/// - Supports verified fingerprints for secure communication
/// - Blocks messages from blocked users
/// - Emergency wipe capability (triple-tap)
///
/// ### User Experience
/// - Smart autocomplete for mentions and commands
/// - Unread message indicators
/// - Connection status tracking
/// - Favorite peers management
///
/// ## Command System
/// Supports IRC-style commands:
/// - `/nick <name>`: Change nickname
/// - `/msg <user> <message>`: Send private message
/// - `/who`: List connected peers
/// - `/slap <user>`: Fun interaction
/// - `/clear`: Clear message history
/// - `/help`: Show available commands
///
/// ## Performance Optimizations
/// - Message batching reduces UI updates
/// - Caches expensive computations (RSSI colors, encryption status)
/// - Debounces autocomplete suggestions
/// - Efficient peer list management
///
/// ## Thread Safety
/// - All @Published properties trigger UI updates on main thread
/// - Background operations use proper queue management
/// - Atomic operations for critical state updates
///
/// ## Usage Example
/// ```swift
/// let viewModel = ChatViewModel()
/// viewModel.nickname = "Alice"
/// viewModel.startServices()
/// viewModel.sendMessage("Hello, mesh network!")
/// ```
///

import Foundation
import SwiftUI
import Combine
import CryptoKit
import CommonCrypto
#if os(iOS)
import UIKit
#endif
import AVFoundation

// Gateway peer information
struct GatewayPeer: Identifiable {
    let id: String  // gateway ID + peer ID
    let nickname: String
    let gatewayId: String
    let gatewayName: String
    let rssi: Int?
    let timestamp: Date
}

/// Manages the application state and business logic for BitChat.
/// Acts as the primary coordinator between UI components and backend services,
/// implementing the BitchatDelegate protocol to handle network events.
class ChatViewModel: ObservableObject {
    // MARK: - Published Properties
    
    @Published var messages: [BitchatMessage] = []
    private let maxMessages = 1337 // Maximum messages before oldest are removed
    @Published var connectedPeers: [String] = []
    
    // MARK: - Message Batching Properties
    
    // Message batching for performance
    private var pendingMessages: [BitchatMessage] = []
    private var pendingPrivateMessages: [String: [BitchatMessage]] = [:] // peerID -> messages
    private var messageBatchTimer: Timer?
    private let messageBatchInterval: TimeInterval = 0.1 // 100ms batching window
    @Published var nickname: String = "" {
        didSet {
            // Trim whitespace whenever nickname is set
            let trimmed = nickname.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed != nickname {
                nickname = trimmed
            }
        }
    }
    @Published var isConnected = false
    @Published var privateChats: [String: [BitchatMessage]] = [:] // peerID -> messages
    @Published var selectedPrivateChatPeer: String? = nil
    private var selectedPrivateChatFingerprint: String? = nil  // Track by fingerprint for persistence across reconnections
    @Published var unreadPrivateMessages: Set<String> = []
    @Published var autocompleteSuggestions: [String] = []
    @Published var showAutocomplete: Bool = false
    @Published var autocompleteRange: NSRange? = nil
    @Published var selectedAutocompleteIndex: Int = 0
    
    // MARK: - Autocomplete Properties
    
    // Autocomplete optimization
    private let mentionRegex = try? NSRegularExpression(pattern: "@([a-zA-Z0-9_]*)$", options: [])
    private var cachedNicknames: [String] = []
    private var lastNicknameUpdate: Date = .distantPast
    
    // Temporary property to fix compilation
    @Published var showPasswordPrompt = false
    
    // MARK: - Services and Storage
    
    var meshService = BluetoothMeshService()
    var gatewayTransport: GatewayTransport!
    @Published var isGatewayConnected = false
    @Published var gatewayName: String?
    @Published var gatewayPeers: [GatewayPeer] = []  // Peers connected through gateways

    private let nicknameKey = "bitchat.nickname"
    private let userDefaults = UserDefaults.standard
    
    // MARK: - Caches
    
    // Caches for expensive computations
    private var rssiColorCache: [String: Color] = [:] // key: "\(rssi)_\(isDark)"
    private var encryptionStatusCache: [String: EncryptionStatus] = [:] // key: peerID
    
    // MARK: - Social Features
    
    @Published var favoritePeers: Set<String> = []  // Now stores public key fingerprints instead of peer IDs
    private var peerIDToPublicKeyFingerprint: [String: String] = [:]  // Maps ephemeral peer IDs to persistent fingerprints
    private var blockedUsers: Set<String> = []  // Stores public key fingerprints of blocked users
    
    // MARK: - Encryption and Security
    
    // Noise Protocol encryption status
    @Published var peerEncryptionStatus: [String: EncryptionStatus] = [:]  // peerID -> encryption status
    @Published var verifiedFingerprints: Set<String> = []  // Set of verified fingerprints
    @Published var showingFingerprintFor: String? = nil  // Currently showing fingerprint sheet for peer
    
    // Messages are naturally ephemeral - no persistent storage
    
    // MARK: - Message Delivery Tracking
    
    // Delivery tracking
    private var deliveryTrackerCancellable: AnyCancellable?
    
    // Track sent read receipts to avoid duplicates
    private var sentReadReceipts: Set<String> = []  // messageID set
    
    // Combine cancellables
    private var cancellables = Set<AnyCancellable>()
    
    // MARK: - Initialization
    
    init() {
        loadNickname()
        loadFavorites()
        loadBlockedUsers()
        loadVerifiedFingerprints()
        
        // Configure mesh service
        meshService.delegate = self
        
        // Initialize gateway transport on main actor
        Task { @MainActor in
            gatewayTransport = GatewayTransport()
            
            // Observe gateway connection status
            gatewayTransport.$isConnected
                .receive(on: DispatchQueue.main)
                .sink { [weak self] isConnected in
                    self?.isGatewayConnected = isConnected
                }
                .store(in: &cancellables)
                
            gatewayTransport.$connectedDeviceName
                .receive(on: DispatchQueue.main)
                .sink { [weak self] name in
                    self?.gatewayName = name
                }
                .store(in: &cancellables)
            
            // Subscribe to gateway status updates
            gatewayTransport.statusUpdatePublisher
                .receive(on: DispatchQueue.main)
                .sink { [weak self] statusInfo in
                    self?.updateGatewayPeers(from: statusInfo)
                }
                .store(in: &cancellables)
                
            // Listen for gateway frames using publisher
            gatewayTransport.framePublisher
                .receive(on: DispatchQueue.main)
                .sink { [weak self] frame in
                    guard let self = self else { return }
                    
                    // Convert frame to string and check if it's a STATUS message
                    if let frameString = String(data: frame, encoding: .utf8) {
                        // Clean the string by removing control characters
                        let cleanedString = frameString.filter { char in
                            let scalar = char.unicodeScalars.first!
                            return scalar.value >= 32 && scalar.value <= 126
                        }
                        
                        // Check if this is a STATUS message
                        if cleanedString.hasPrefix("STATUS|") {
                            // Parse and update gateway peers
                            if let statusInfo = parseGatewayStatus(cleanedString) {
                                updateGatewayPeers(from: statusInfo)
                            }
                            // Don't process STATUS messages as chat messages
                            return
                        }
                        
                        // Ignore other debug messages
                        if cleanedString.hasPrefix("Echo:") || cleanedString.hasPrefix("Heartbeat:") {
                            return
                        }
                    }
                    
                    // Log non-STATUS/debug frames for debugging
                    let frameString = String(data: frame, encoding: .utf8) ?? frame.hexEncodedString()
                    print("ChatViewModel: Received gateway frame (\(frame.count) bytes): \(frameString)")
                    
                    // Try to parse as BitChat message
                    if let message = BitchatMessage.fromBinaryPayload(frame) {
                        // Mark as received through gateway
                        message.sentViaGateway = true
                        
                        // Process the message
                        didReceiveMessage(message)
                        
                        print("ChatViewModel: Successfully processed BitChat message via gateway from \(message.sender)")
                    } else {
                        // Not a BitChat message - this is expected for some message types
                        print("ChatViewModel: Frame is not a valid BitChat message")
                    }
                }
                .store(in: &cancellables)
                
            // Start gateway transport
            await gatewayTransport.start()
        }
        
        // Start mesh service
        meshService.startServices()
        
        // Subscribe to delivery status updates
        deliveryTrackerCancellable = DeliveryTracker.shared.deliveryStatusUpdated
            .receive(on: DispatchQueue.main)
            .sink { [weak self] (messageID, status) in
                self?.updateMessageDeliveryStatus(messageID, status: status)
            }
        
        // Log startup info
        
        // Log fingerprint after a delay to ensure encryption service is ready
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            if let self = self {
                _ = self.getMyFingerprint()
            }
        }
        
        // Set up message retry service
        MessageRetryService.shared.meshService = meshService
        
        // Set up Noise encryption callbacks
        setupNoiseCallbacks()
        
        // Request notification permission
        NotificationService.shared.requestAuthorization()
        
        // When app becomes active, send read receipts for visible messages
        #if os(macOS)
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appDidBecomeActive),
            name: NSApplication.didBecomeActiveNotification,
            object: nil
        )
        
        // Add app lifecycle observers to save data
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appWillResignActive),
            name: NSApplication.willResignActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appWillTerminate),
            name: NSApplication.willTerminateNotification,
            object: nil
        )
        #else
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appDidBecomeActive),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
        
        // Add screenshot detection for iOS
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(userDidTakeScreenshot),
            name: UIApplication.userDidTakeScreenshotNotification,
            object: nil
        )
        
        // Add app lifecycle observers to save data
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appWillResignActive),
            name: UIApplication.willResignActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appWillTerminate),
            name: UIApplication.willTerminateNotification,
            object: nil
        )
        #endif
    }
    
    // MARK: - Deinitialization
    
    deinit {
        // Clean up timer
        messageBatchTimer?.invalidate()
        
        // Force immediate save
        userDefaults.synchronize()
    }
    
    // MARK: - Nickname Management
    
    private func loadNickname() {
        if let savedNickname = userDefaults.string(forKey: nicknameKey) {
            // Trim whitespace when loading
            nickname = savedNickname.trimmingCharacters(in: CharacterSet.whitespacesAndNewlines)
        } else {
            nickname = "anon\(Int.random(in: 1000...9999))"
            saveNickname()
        }
    }
    
    func saveNickname() {
        userDefaults.set(nickname, forKey: nicknameKey)
        userDefaults.synchronize() // Force immediate save
        
        // Send announce with new nickname to all peers
        meshService.sendBroadcastAnnounce()
    }
    
    func validateAndSaveNickname() {
        // Trim whitespace from nickname
        let trimmed = nickname.trimmingCharacters(in: .whitespacesAndNewlines)
        
        // Check if nickname is empty after trimming
        if trimmed.isEmpty {
            nickname = "anon\(Int.random(in: 1000...9999))"
        } else {
            nickname = trimmed
        }
        saveNickname()
    }
    
    // MARK: - Favorites Management
    
    private func loadFavorites() {
        // Load favorites from secure storage
        favoritePeers = SecureIdentityStateManager.shared.getFavorites()
    }
    
    private func saveFavorites() {
        // Favorites are now saved automatically in SecureIdentityStateManager
        // This method is kept for compatibility
    }
    
    // MARK: - Blocked Users Management
    
    private func loadBlockedUsers() {
        // Load blocked users from secure storage
        let allIdentities = SecureIdentityStateManager.shared.getAllSocialIdentities()
        blockedUsers = Set(allIdentities.filter { $0.isBlocked }.map { $0.fingerprint })
    }
    
    private func saveBlockedUsers() {
        // Blocked users are now saved automatically in SecureIdentityStateManager
        // This method is kept for compatibility
    }
    
    
    func toggleFavorite(peerID: String) {
        // First try to get fingerprint from mesh service (supports peer ID rotation)
        var fingerprint: String? = meshService.getFingerprint(for: peerID)
        
        // Fallback to local mapping if not found in mesh service
        if fingerprint == nil {
            fingerprint = peerIDToPublicKeyFingerprint[peerID]
        }
        
        guard let fp = fingerprint else {
            return
        }
        
        let isFavorite = SecureIdentityStateManager.shared.isFavorite(fingerprint: fp)
        SecureIdentityStateManager.shared.setFavorite(fp, isFavorite: !isFavorite)
        
        // Update local set for UI
        if isFavorite {
            favoritePeers.remove(fp)
        } else {
            favoritePeers.insert(fp)
        }
    }
    
    func isFavorite(peerID: String) -> Bool {
        // First try to get fingerprint from mesh service (supports peer ID rotation)
        var fingerprint: String? = meshService.getFingerprint(for: peerID)
        
        // Fallback to local mapping if not found in mesh service
        if fingerprint == nil {
            fingerprint = peerIDToPublicKeyFingerprint[peerID]
        }
        
        guard let fp = fingerprint else {
            return false
        }
        
        return SecureIdentityStateManager.shared.isFavorite(fingerprint: fp)
    }
    
    // MARK: - Public Key and Identity Management
    
    // Called when we receive a peer's public key
    func registerPeerPublicKey(peerID: String, publicKeyData: Data) {
        // Create a fingerprint from the public key (full SHA256, not truncated)
        let fingerprintStr = SHA256.hash(data: publicKeyData)
            .compactMap { String(format: "%02x", $0) }
            .joined()
        
        // Only register if not already registered
        if peerIDToPublicKeyFingerprint[peerID] != fingerprintStr {
            peerIDToPublicKeyFingerprint[peerID] = fingerprintStr
        }
        
        // Update identity state manager with handshake completion
        SecureIdentityStateManager.shared.updateHandshakeState(peerID: peerID, state: .completed(fingerprint: fingerprintStr))
        
        // Update encryption status now that we have the fingerprint
        updateEncryptionStatus(for: peerID)
        
        // Check if we have a claimed nickname for this peer
        let peerNicknames = meshService.getPeerNicknames()
        if let nickname = peerNicknames[peerID], nickname != "Unknown" && nickname != "anon\(peerID.prefix(4))" {
            // Update or create social identity with the claimed nickname
            if var identity = SecureIdentityStateManager.shared.getSocialIdentity(for: fingerprintStr) {
                identity.claimedNickname = nickname
                SecureIdentityStateManager.shared.updateSocialIdentity(identity)
            } else {
                let newIdentity = SocialIdentity(
                    fingerprint: fingerprintStr,
                    localPetname: nil,
                    claimedNickname: nickname,
                    trustLevel: .casual,
                    isFavorite: false,
                    isBlocked: false,
                    notes: nil
                )
                SecureIdentityStateManager.shared.updateSocialIdentity(newIdentity)
            }
        }
        
        // Check if this peer is the one we're in a private chat with
        updatePrivateChatPeerIfNeeded()
    }
    
    private func isPeerBlocked(_ peerID: String) -> Bool {
        // Check if we have the public key fingerprint for this peer
        if let fingerprint = peerIDToPublicKeyFingerprint[peerID] {
            return SecureIdentityStateManager.shared.isBlocked(fingerprint: fingerprint)
        }
        
        // Try to get fingerprint from mesh service
        if let fingerprint = meshService.getPeerFingerprint(peerID) {
            return SecureIdentityStateManager.shared.isBlocked(fingerprint: fingerprint)
        }
        
        return false
    }
    
    // Helper method to find current peer ID for a fingerprint
    private func getCurrentPeerIDForFingerprint(_ fingerprint: String) -> String? {
        // Search through all connected peers to find the one with matching fingerprint
        for peerID in connectedPeers {
            if let mappedFingerprint = peerIDToPublicKeyFingerprint[peerID],
               mappedFingerprint == fingerprint {
                return peerID
            }
        }
        return nil
    }
    
    // Helper method to update selectedPrivateChatPeer if fingerprint matches
    private func updatePrivateChatPeerIfNeeded() {
        guard let chatFingerprint = selectedPrivateChatFingerprint else { return }
        
        // Find current peer ID for the fingerprint
        if let currentPeerID = getCurrentPeerIDForFingerprint(chatFingerprint) {
            // Update the selected peer if it's different
            if let oldPeerID = selectedPrivateChatPeer, oldPeerID != currentPeerID {
                // Migrate messages from old peer ID to new peer ID
                if let oldMessages = privateChats[oldPeerID] {
                    if privateChats[currentPeerID] == nil {
                        privateChats[currentPeerID] = []
                    }
                    privateChats[currentPeerID]?.append(contentsOf: oldMessages)
                    trimPrivateChatMessagesIfNeeded(for: currentPeerID)
                    privateChats.removeValue(forKey: oldPeerID)
                }
                
                // Migrate unread status
                if unreadPrivateMessages.contains(oldPeerID) {
                    unreadPrivateMessages.remove(oldPeerID)
                    unreadPrivateMessages.insert(currentPeerID)
                }
                
                selectedPrivateChatPeer = currentPeerID
            } else if selectedPrivateChatPeer == nil {
                // Just set the peer ID if we don't have one
                selectedPrivateChatPeer = currentPeerID
            }
            
            // Clear unread messages for the current peer ID
            unreadPrivateMessages.remove(currentPeerID)
        }
    }
    
    // MARK: - Message Sending
    
    private func parseMentions(from content: String) -> [String] {
        let pattern = "@([a-zA-Z0-9_]+)"
        let regex = try? NSRegularExpression(pattern: pattern, options: [])
        let matches = regex?.matches(in: content, options: [], range: NSRange(location: 0, length: content.count)) ?? []
        
        var mentions: [String] = []
        for match in matches {
            if let range = Range(match.range(at: 1), in: content) {
                let mention = String(content[range])
                if !mentions.contains(mention) {
                    mentions.append(mention)
                }
            }
        }
        return mentions
    }
    
    private func handleCommand(_ command: String) {
        // Simple command handling - can be extended
        let parts = command.split(separator: " ", maxSplits: 1)
        guard let cmd = parts.first else { return }
        
        switch cmd.lowercased() {
        case "/help":
            let helpMessage = BitchatMessage(
                sender: "system",
                content: "Available commands: /help, /clear, /nick <nickname>",
                timestamp: Date(),
                isRelay: false
            )
            messages.append(helpMessage)
            
        case "/clear":
            messages.removeAll()
            
        case "/nick":
            if parts.count > 1 {
                let newNick = String(parts[1])
                nickname = newNick
                saveNickname()
            }
            
        default:
            let errorMessage = BitchatMessage(
                sender: "system",
                content: "Unknown command: \(cmd)",
                timestamp: Date(),
                isRelay: false
            )
            messages.append(errorMessage)
        }
    }
    
    /// Sends a message through the BitChat network.
    /// - Parameter content: The message content to send
    /// - Note: Automatically handles command processing if content starts with '/'
    ///         Routes to private chat if one is selected, otherwise broadcasts
    func sendMessage(_ content: String) {
        guard !content.isEmpty else { return }
        
        // Check for commands
        if content.hasPrefix("/") {
            handleCommand(content)
            return
        }
        
        if selectedPrivateChatPeer != nil {
            // Update peer ID in case it changed due to reconnection
            updatePrivateChatPeerIfNeeded()
            
            if let selectedPeer = selectedPrivateChatPeer {
                // Send as private message
                sendPrivateMessage(content, to: selectedPeer)
            } else {
            }
        } else {
            // Parse mentions from the content
            let mentions = parseMentions(from: content)
            
            // Add message to local display
            let message = BitchatMessage(
                sender: nickname,
                content: content,
                timestamp: Date(),
                isRelay: false,
                originalSender: nil,
                isPrivate: false,
                recipientNickname: nil,
                senderPeerID: meshService.myPeerID,
                mentions: mentions.isEmpty ? nil : mentions
            )
            
            // Mark as sent via gateway if no direct peers
            message.sentViaGateway = connectedPeers.isEmpty && isGatewayConnected
            
            // Add to main messages immediately for user feedback
            messages.append(message)
            trimMessagesIfNeeded()
            
            // Force immediate UI update for user's own messages
            objectWillChange.send()
            
            // Send via mesh with mentions
            meshService.sendMessage(content, mentions: mentions)
            
            // Also send via gateway transport for extended range if connected
            if isGatewayConnected {
                Task { @MainActor in
                    guard let gatewayTransport = gatewayTransport else {
                        print("ChatViewModel: Gateway transport not initialized yet")
                        return
                    }
                    
                    do {
                        // Create a BitChat message in binary format for gateway
                        let bitchatMessage = BitchatMessage(
                            sender: nickname,
                            content: content,
                            timestamp: Date(),
                            isRelay: false,
                            originalSender: nil,
                            mentions: mentions
                        )
                        
                        // Convert to binary payload
                        guard let binaryPayload = bitchatMessage.toBinaryPayload() else {
                            print("ChatViewModel: Failed to encode message to binary format")
                            return
                        }
                        
                        try await gatewayTransport.sendOpaque(binaryPayload)
                        print("ChatViewModel: Message forwarded to gateway (\(binaryPayload.count) bytes)")
                    } catch {
                        print("ChatViewModel: Failed to forward message to gateway: \(error)")
                    }
                }
            }
        }
    }
    
    /// Sends an encrypted private message to a specific peer.
    /// - Parameters:
    ///   - content: The message content to encrypt and send
    ///   - peerID: The recipient's peer ID
    /// - Note: Automatically establishes Noise encryption if not already active
    func sendPrivateMessage(_ content: String, to peerID: String) {
        guard !content.isEmpty else { return }
        guard let recipientNickname = meshService.getPeerNicknames()[peerID] else { 
            return 
        }
        
        // Check if the recipient is blocked
        if isPeerBlocked(peerID) {
            let systemMessage = BitchatMessage(
                sender: "system",
                content: "cannot send message to \(recipientNickname): user is blocked.",
                timestamp: Date(),
                isRelay: false
            )
            messages.append(systemMessage)
            return
        }
        
        // IMPORTANT: When sending a message, it means we're viewing this chat
        // Send read receipts for any delivered messages from this peer
        markPrivateMessagesAsRead(from: peerID)
        
        // Create the message locally
        let message = BitchatMessage(
            sender: nickname,
            content: content,
            timestamp: Date(),
            isRelay: false,
            originalSender: nil,
            isPrivate: true,
            recipientNickname: recipientNickname,
            senderPeerID: meshService.myPeerID,
            deliveryStatus: .sending
        )
        
        // Add to our private chat history
        if privateChats[peerID] == nil {
            privateChats[peerID] = []
        }
        privateChats[peerID]?.append(message)
        trimPrivateChatMessagesIfNeeded(for: peerID)
        
        // Track the message for delivery confirmation
        let isFavorite = isFavorite(peerID: peerID)
        DeliveryTracker.shared.trackMessage(message, recipientID: peerID, recipientNickname: recipientNickname, isFavorite: isFavorite)
        
        // Immediate UI update for user's own messages
        objectWillChange.send()
        
        // Send via mesh with the same message ID
        meshService.sendPrivateMessage(content, to: peerID, recipientNickname: recipientNickname, messageID: message.id)
    }
    
    // MARK: - Private Chat Management
    
    /// Initiates a private chat session with a peer.
    /// - Parameter peerID: The peer's ID to start chatting with
    /// - Note: Switches the UI to private chat mode and loads message history
    func startPrivateChat(with peerID: String) {
        let peerNickname = meshService.getPeerNicknames()[peerID] ?? "unknown"
        
        // Check if the peer is blocked
        if isPeerBlocked(peerID) {
            let systemMessage = BitchatMessage(
                sender: "system",
                content: "cannot start chat with \(peerNickname): user is blocked.",
                timestamp: Date(),
                isRelay: false
            )
            messages.append(systemMessage)
            return
        }
        
        // Trigger handshake if we don't have a session yet
        let sessionState = meshService.getNoiseSessionState(for: peerID)
        switch sessionState {
        case .none, .failed:
            // Initiate handshake when opening PM
            meshService.triggerHandshake(with: peerID)
        default:
            break
        }
        
        selectedPrivateChatPeer = peerID
        // Also track by fingerprint for persistence across reconnections
        selectedPrivateChatFingerprint = peerIDToPublicKeyFingerprint[peerID]
        unreadPrivateMessages.remove(peerID)
        
        // Check if we need to migrate messages from an old peer ID
        // This happens when peer IDs change between sessions
        if privateChats[peerID] == nil || privateChats[peerID]?.isEmpty == true {
            
            // Look for messages from this nickname under other peer IDs
            var migratedMessages: [BitchatMessage] = []
            var oldPeerIDsToRemove: [String] = []
            
            for (oldPeerID, messages) in privateChats {
                if oldPeerID != peerID {
                    // Check if any messages in this chat are from the peer's nickname
                    // Check if this chat contains messages with this peer
                    let messagesWithPeer = messages.filter { msg in
                        // Message is FROM the peer to us
                        (msg.sender == peerNickname && msg.sender != nickname) ||
                        // OR message is FROM us TO the peer
                        (msg.sender == nickname && (msg.recipientNickname == peerNickname || 
                         // Also check if this was a private message in a chat that only has us and one other person
                         (msg.isPrivate && messages.allSatisfy { m in 
                             m.sender == nickname || m.sender == peerNickname 
                         })))
                    }
                    
                    if !messagesWithPeer.isEmpty {
                        
                        // Check if ALL messages in this chat are between us and this peer
                        let allMessagesAreWithPeer = messages.allSatisfy { msg in
                            (msg.sender == peerNickname || msg.sender == nickname) &&
                            (msg.recipientNickname == nil || msg.recipientNickname == peerNickname || msg.recipientNickname == nickname)
                        }
                        
                        if allMessagesAreWithPeer {
                            // This entire chat history belongs to this peer, migrate it all
                            migratedMessages.append(contentsOf: messages)
                            oldPeerIDsToRemove.append(oldPeerID)
                        }
                    }
                }
            }
            
            // Remove old peer ID entries that were fully migrated
            for oldPeerID in oldPeerIDsToRemove {
                privateChats.removeValue(forKey: oldPeerID)
                unreadPrivateMessages.remove(oldPeerID)
            }
            
            // Initialize chat history with migrated messages if any
            if !migratedMessages.isEmpty {
                privateChats[peerID] = migratedMessages.sorted { $0.timestamp < $1.timestamp }
                trimPrivateChatMessagesIfNeeded(for: peerID)
            } else {
                privateChats[peerID] = []
            }
        }
        
        _ = privateChats[peerID] ?? []
        
        // Send read receipts for unread messages from this peer
        // Add a small delay to ensure UI has updated
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
            self?.markPrivateMessagesAsRead(from: peerID)
        }
        
        // Also try immediately in case messages are already there
        markPrivateMessagesAsRead(from: peerID)
    }
    
    func endPrivateChat() {
        selectedPrivateChatPeer = nil
        selectedPrivateChatFingerprint = nil
    }
    
    // MARK: - Message Retry Handling
    
    @objc private func handleRetryMessage(_ notification: Notification) {
        guard let messageID = notification.userInfo?["messageID"] as? String else { return }
        
        // Find the message to retry
        if let message = messages.first(where: { $0.id == messageID }) {
            SecureLogger.log("Retrying message \(messageID) to \(message.recipientNickname ?? "unknown")", 
                           category: SecureLogger.session, level: .info)
            
            // Resend the message through mesh service
            if message.isPrivate,
               let peerID = getPeerIDForNickname(message.recipientNickname ?? "") {
                // Update status to sending
                updateMessageDeliveryStatus(messageID, status: DeliveryStatus.sending)
                
                // Resend via mesh service
                meshService.sendMessage(message.content, 
                                      mentions: message.mentions ?? [], 
                                      to: peerID,
                                      messageID: messageID,
                                      timestamp: message.timestamp)
            }
        }
    }
    
    // MARK: - App Lifecycle
    
    @objc private func appDidBecomeActive() {
        // When app becomes active, send read receipts for visible private chat
        if let peerID = selectedPrivateChatPeer {
            // Try immediately
            self.markPrivateMessagesAsRead(from: peerID)
            // And again with a delay
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                self.markPrivateMessagesAsRead(from: peerID)
            }
        }
    }
    
    @objc private func userDidTakeScreenshot() {
        // Send screenshot notification based on current context
        let screenshotMessage = "* \(nickname) took a screenshot *"
        
        if let peerID = selectedPrivateChatPeer {
            // In private chat - send to the other person
            if let peerNickname = meshService.getPeerNicknames()[peerID] {
                // Only send screenshot notification if we have an established session
                // This prevents triggering handshake requests for screenshot notifications
                let sessionState = meshService.getNoiseSessionState(for: peerID)
                switch sessionState {
                case .established:
                    // Send the message directly without going through sendPrivateMessage to avoid local echo
                    meshService.sendPrivateMessage(screenshotMessage, to: peerID, recipientNickname: peerNickname)
                default:
                    // Don't send screenshot notification if no session exists
                    SecureLogger.log("Skipping screenshot notification to \(peerID) - no established session", category: SecureLogger.security, level: .debug)
                }
            }
            
            // Show local notification immediately as system message
            let localNotification = BitchatMessage(
                sender: "system",
                content: "you took a screenshot",
                timestamp: Date(),
                isRelay: false,
                originalSender: nil,
                isPrivate: true,
                recipientNickname: meshService.getPeerNicknames()[peerID],
                senderPeerID: meshService.myPeerID
            )
            if privateChats[peerID] == nil {
                privateChats[peerID] = []
            }
            privateChats[peerID]?.append(localNotification)
            trimPrivateChatMessagesIfNeeded(for: peerID)
            
        } else {
            // In public chat - send to everyone
            meshService.sendMessage(screenshotMessage, mentions: [])
            
            // Show local notification immediately as system message
            let localNotification = BitchatMessage(
                sender: "system",
                content: "you took a screenshot",
                timestamp: Date(),
                isRelay: false
            )
            // System messages can be batched
            addMessageToBatch(localNotification)
        }
    }
    
    @objc private func appWillResignActive() {
        // Flush any pending messages when app goes to background
        flushMessageBatchImmediately()
        
        userDefaults.synchronize()
    }
    
    @objc func applicationWillTerminate() {
        // Flush any pending messages immediately
        flushMessageBatchImmediately()
        
        // Force save any pending identity changes (verifications, favorites, etc)
        SecureIdentityStateManager.shared.forceSave()
        
        // Verify identity key is still there
        _ = KeychainManager.shared.verifyIdentityKeyExists()
        
        userDefaults.synchronize()
        
        // Verify identity key after save
        _ = KeychainManager.shared.verifyIdentityKeyExists()
    }
    
    @objc private func appWillTerminate() {
        // Flush any pending messages immediately
        flushMessageBatchImmediately()
        
        userDefaults.synchronize()
    }
    
    func markPrivateMessagesAsRead(from peerID: String) {
        // Get the nickname for this peer
        let peerNickname = meshService.getPeerNicknames()[peerID] ?? ""
        
        // First ensure we have the latest messages (in case of migration)
        if let messages = privateChats[peerID], !messages.isEmpty {
        } else {
            
            // Look through ALL private chats to find messages from this nickname
            for (_, chatMessages) in privateChats {
                let relevantMessages = chatMessages.filter { msg in
                    msg.sender == peerNickname && msg.sender != nickname
                }
                if !relevantMessages.isEmpty {
                }
            }
        }
        
        guard let messages = privateChats[peerID], !messages.isEmpty else { 
            return 
        }
        
        
        // Find messages from the peer that haven't been read yet
        var readReceiptsSent = 0
        for (_, message) in messages.enumerated() {
            // Only send read receipts for messages from the other peer (not our own)
            // Check multiple conditions to ensure we catch all messages from the peer
            let isOurMessage = message.sender == nickname
            let isFromPeerByNickname = !peerNickname.isEmpty && message.sender == peerNickname
            let isFromPeerByID = message.senderPeerID == peerID
            let isPrivateToUs = message.isPrivate && message.recipientNickname == nickname
            
            // This is a message FROM the peer if it's not from us AND (matches nickname OR peer ID OR is private to us)
            let isFromPeer = !isOurMessage && (isFromPeerByNickname || isFromPeerByID || isPrivateToUs)
            
            if message.id == message.id { // Always true, for debugging
            }
            
            if isFromPeer {
                if let status = message.deliveryStatus {
                    switch status {
                    case .sent, .delivered:
                        // Create and send read receipt for sent or delivered messages
                        // Check if we've already sent a receipt for this message
                        if !sentReadReceipts.contains(message.id) {
                            // Send to the CURRENT peer ID, not the old senderPeerID which may have changed
                            let receipt = ReadReceipt(
                                originalMessageID: message.id,
                                readerID: meshService.myPeerID,
                                readerNickname: nickname
                            )
                            meshService.sendReadReceipt(receipt, to: peerID)
                            sentReadReceipts.insert(message.id)
                            readReceiptsSent += 1
                        } else {
                        }
                    case .read:
                        // Already read, no need to send another receipt
                        break
                    default:
                        // Message not yet delivered, can't mark as read
                        break
                    }
                } else {
                    // No delivery status - this might be an older message
                    // Send read receipt anyway for backwards compatibility
                    if !sentReadReceipts.contains(message.id) {
                        let receipt = ReadReceipt(
                            originalMessageID: message.id,
                            readerID: meshService.myPeerID,
                            readerNickname: nickname
                        )
                        meshService.sendReadReceipt(receipt, to: peerID)
                        sentReadReceipts.insert(message.id)
                        readReceiptsSent += 1
                    } else {
                    }
                }
            } else {
            }
        }
        
    }
    
    func getPrivateChatMessages(for peerID: String) -> [BitchatMessage] {
        let messages = privateChats[peerID] ?? []
        if !messages.isEmpty {
        }
        return messages
    }
    
    func getPeerIDForNickname(_ nickname: String) -> String? {
        let nicknames = meshService.getPeerNicknames()
        return nicknames.first(where: { $0.value == nickname })?.key
    }
    
    
    // MARK: - Emergency Functions
    
    // PANIC: Emergency data clearing for activist safety
    func panicClearAllData() {
        // Flush any pending messages immediately before clearing
        flushMessageBatchImmediately()
        
        // Clear all messages
        messages.removeAll()
        privateChats.removeAll()
        unreadPrivateMessages.removeAll()
        
        // First run aggressive cleanup to get rid of all legacy items
        _ = KeychainManager.shared.aggressiveCleanupLegacyItems()
        
        // Then delete all current keychain data
        _ = KeychainManager.shared.deleteAllKeychainData()
        
        // Clear UserDefaults identity fallbacks
        userDefaults.removeObject(forKey: "bitchat.noiseIdentityKey")
        userDefaults.removeObject(forKey: "bitchat.messageRetentionKey")
        
        // Clear verified fingerprints
        verifiedFingerprints.removeAll()
        // Verified fingerprints are cleared when identity data is cleared below
        
        // Clear message retry queue
        MessageRetryService.shared.clearRetryQueue()
        
        
        // Reset nickname to anonymous
        nickname = "anon\(Int.random(in: 1000...9999))"
        saveNickname()
        
        // Clear favorites
        favoritePeers.removeAll()
        peerIDToPublicKeyFingerprint.removeAll()
        
        // Clear identity data from secure storage
        SecureIdentityStateManager.shared.clearAllIdentityData()
        
        // Clear autocomplete state
        autocompleteSuggestions.removeAll()
        showAutocomplete = false
        autocompleteRange = nil
        selectedAutocompleteIndex = 0
        
        // Clear selected private chat
        selectedPrivateChatPeer = nil
        selectedPrivateChatFingerprint = nil
        
        // Clear read receipt tracking
        sentReadReceipts.removeAll()
        
        // Clear all caches
        invalidateEncryptionCache()
        invalidateRSSIColorCache()
        
        // Disconnect from all peers and clear persistent identity
        // This will force creation of a new identity (new fingerprint) on next launch
        meshService.emergencyDisconnectAll()
        
        // Force immediate UserDefaults synchronization
        userDefaults.synchronize()
        
        // Force UI update
        objectWillChange.send()
        
    }
    
    
    
    // MARK: - Formatting Helpers
    
    func formatTimestamp(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: date)
    }
    
    func getRSSIColor(rssi: Int, colorScheme: ColorScheme) -> Color {
        let isDark = colorScheme == .dark
        let cacheKey = "\(rssi)_\(isDark)"
        
        // Check cache first
        if let cachedColor = rssiColorCache[cacheKey] {
            return cachedColor
        }
        
        // RSSI typically ranges from -30 (excellent) to -90 (poor)
        // We'll map this to colors from green (strong) to red (weak)
        
        let color: Color
        if rssi >= -50 {
            // Excellent signal: bright green
            color = isDark ? Color(red: 0.0, green: 1.0, blue: 0.0) : Color(red: 0.0, green: 0.7, blue: 0.0)
        } else if rssi >= -60 {
            // Good signal: green-yellow
            color = isDark ? Color(red: 0.5, green: 1.0, blue: 0.0) : Color(red: 0.3, green: 0.7, blue: 0.0)
        } else if rssi >= -70 {
            // Fair signal: yellow
            color = isDark ? Color(red: 1.0, green: 1.0, blue: 0.0) : Color(red: 0.7, green: 0.7, blue: 0.0)
        } else if rssi >= -80 {
            // Weak signal: orange
            color = isDark ? Color(red: 1.0, green: 0.6, blue: 0.0) : Color(red: 0.8, green: 0.4, blue: 0.0)
        } else {
            // Poor signal: red
            color = isDark ? Color(red: 1.0, green: 0.2, blue: 0.2) : Color(red: 0.8, green: 0.0, blue: 0.0)
        }
        
        // Cache the result
        rssiColorCache[cacheKey] = color
        return color
    }
    
    // MARK: - Autocomplete
    
    func updateAutocomplete(for text: String, cursorPosition: Int) {
        // Quick early exit for empty text
        guard cursorPosition > 0 else {
            if showAutocomplete {
                showAutocomplete = false
                autocompleteSuggestions = []
                autocompleteRange = nil
            }
            return
        }
        
        // Find @ symbol before cursor
        let beforeCursor = String(text.prefix(cursorPosition))
        
        // Use cached regex
        guard let regex = mentionRegex,
              let match = regex.firstMatch(in: beforeCursor, options: [], range: NSRange(location: 0, length: beforeCursor.count)) else {
            if showAutocomplete {
                showAutocomplete = false
                autocompleteSuggestions = []
                autocompleteRange = nil
            }
            return
        }
        
        // Extract the partial nickname
        let partialRange = match.range(at: 1)
        guard let range = Range(partialRange, in: beforeCursor) else {
            if showAutocomplete {
                showAutocomplete = false
                autocompleteSuggestions = []
                autocompleteRange = nil
            }
            return
        }
        
        let partial = String(beforeCursor[range]).lowercased()
        
        // Update cached nicknames only if peer list changed (check every 1 second max)
        let now = Date()
        if now.timeIntervalSince(lastNicknameUpdate) > 1.0 || cachedNicknames.isEmpty {
            let peerNicknames = meshService.getPeerNicknames()
            cachedNicknames = Array(peerNicknames.values).sorted()
            lastNicknameUpdate = now
        }
        
        // Filter suggestions using cached nicknames
        let suggestions = cachedNicknames.filter { nick in
            nick.lowercased().hasPrefix(partial)
        }
        
        // Batch UI updates
        if !suggestions.isEmpty {
            // Only update if suggestions changed
            if autocompleteSuggestions != suggestions {
                autocompleteSuggestions = suggestions
            }
            if !showAutocomplete {
                showAutocomplete = true
            }
            if autocompleteRange != match.range(at: 0) {
                autocompleteRange = match.range(at: 0)
            }
            selectedAutocompleteIndex = 0
        } else {
            if showAutocomplete {
                showAutocomplete = false
                autocompleteSuggestions = []
                autocompleteRange = nil
                selectedAutocompleteIndex = 0
            }
        }
    }
    
    func completeNickname(_ nickname: String, in text: inout String) -> Int {
        guard let range = autocompleteRange else { return text.count }
        
        // Replace the @partial with @nickname
        let nsText = text as NSString
        let newText = nsText.replacingCharacters(in: range, with: "@\(nickname) ")
        text = newText
        
        // Hide autocomplete
        showAutocomplete = false
        autocompleteSuggestions = []
        autocompleteRange = nil
        selectedAutocompleteIndex = 0
        
        // Return new cursor position (after the space)
        return range.location + nickname.count + 2
    }
    
    // MARK: - Message Formatting
    
    func getSenderColor(for message: BitchatMessage, colorScheme: ColorScheme) -> Color {
        let isDark = colorScheme == .dark
        let primaryColor = isDark ? Color.green : Color(red: 0, green: 0.5, blue: 0)
        
        // Always use the same color for all senders - no RSSI-based coloring
        return primaryColor
    }
    
    
    func formatMessageContent(_ message: BitchatMessage, colorScheme: ColorScheme) -> AttributedString {
        let isDark = colorScheme == .dark
        let contentText = message.content
        var processedContent = AttributedString()
        
        // Regular expressions for mentions and hashtags
        let mentionPattern = "@([a-zA-Z0-9_]+)"
        let hashtagPattern = "#([a-zA-Z0-9_]+)"
        
        let mentionRegex = try? NSRegularExpression(pattern: mentionPattern, options: [])
        let hashtagRegex = try? NSRegularExpression(pattern: hashtagPattern, options: [])
        
        let mentionMatches = mentionRegex?.matches(in: contentText, options: [], range: NSRange(location: 0, length: contentText.count)) ?? []
        let hashtagMatches = hashtagRegex?.matches(in: contentText, options: [], range: NSRange(location: 0, length: contentText.count)) ?? []
        
        // Combine and sort all matches
        var allMatches: [(range: NSRange, type: String)] = []
        for match in mentionMatches {
            allMatches.append((match.range(at: 0), "mention"))
        }
        for match in hashtagMatches {
            allMatches.append((match.range(at: 0), "hashtag"))
        }
        allMatches.sort { $0.range.location < $1.range.location }
        
        var lastEndIndex = contentText.startIndex
        
        for (matchRange, matchType) in allMatches {
            // Add text before the match
            if let range = Range(matchRange, in: contentText) {
                let beforeText = String(contentText[lastEndIndex..<range.lowerBound])
                if !beforeText.isEmpty {
                    var normalStyle = AttributeContainer()
                    normalStyle.font = .system(size: 14, design: .monospaced)
                    normalStyle.foregroundColor = isDark ? Color.white : Color.black
                    processedContent.append(AttributedString(beforeText).mergingAttributes(normalStyle))
                }
                
                // Add the match with appropriate styling
                let matchText = String(contentText[range])
                var matchStyle = AttributeContainer()
                matchStyle.font = .system(size: 14, weight: .semibold, design: .monospaced)
                
                if matchType == "mention" {
                    matchStyle.foregroundColor = Color.orange
                } else {
                    // Hashtag
                    matchStyle.foregroundColor = Color.blue
                    matchStyle.underlineStyle = .single
                }
                
                processedContent.append(AttributedString(matchText).mergingAttributes(matchStyle))
                
                lastEndIndex = range.upperBound
            }
        }
        
        // Add any remaining text
        if lastEndIndex < contentText.endIndex {
            let remainingText = String(contentText[lastEndIndex...])
            var normalStyle = AttributeContainer()
            normalStyle.font = .system(size: 14, design: .monospaced)
            normalStyle.foregroundColor = isDark ? Color.white : Color.black
            processedContent.append(AttributedString(remainingText).mergingAttributes(normalStyle))
        }
        
        return processedContent
    }
    
    func formatMessageAsText(_ message: BitchatMessage, colorScheme: ColorScheme) -> AttributedString {
        // Check cache first
        let isDark = colorScheme == .dark
        if let cachedText = message.getCachedFormattedText(isDark: isDark) {
            return cachedText
        }
        
        // Not cached, format the message
        var result = AttributedString()
        
        let primaryColor = isDark ? Color.green : Color(red: 0, green: 0.5, blue: 0)
        let secondaryColor = primaryColor.opacity(0.7)
        
        // Timestamp
        let timestamp = AttributedString("[\(formatTimestamp(message.timestamp))] ")
        var timestampStyle = AttributeContainer()
        timestampStyle.foregroundColor = message.sender == "system" ? Color.gray : secondaryColor
        timestampStyle.font = .system(size: 12, design: .monospaced)
        result.append(timestamp.mergingAttributes(timestampStyle))
        
        if message.sender != "system" {
            // Sender
            let sender = AttributedString("<@\(message.sender)> ")
            var senderStyle = AttributeContainer()
            
            // Use consistent color for all senders
            senderStyle.foregroundColor = primaryColor
            // Bold the user's own nickname
            let fontWeight: Font.Weight = message.sender == nickname ? .bold : .medium
            senderStyle.font = .system(size: 14, weight: fontWeight, design: .monospaced)
            result.append(sender.mergingAttributes(senderStyle))
            
            // Process content with hashtags and mentions
            let content = message.content
            
            let hashtagPattern = "#([a-zA-Z0-9_]+)"
            let mentionPattern = "@([a-zA-Z0-9_]+)"
            
            let hashtagRegex = try? NSRegularExpression(pattern: hashtagPattern, options: [])
            let mentionRegex = try? NSRegularExpression(pattern: mentionPattern, options: [])
            
            // Use NSDataDetector for URL detection
            let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)
            
            let hashtagMatches = hashtagRegex?.matches(in: content, options: [], range: NSRange(location: 0, length: content.count)) ?? []
            let mentionMatches = mentionRegex?.matches(in: content, options: [], range: NSRange(location: 0, length: content.count)) ?? []
            let urlMatches = detector?.matches(in: content, options: [], range: NSRange(location: 0, length: content.count)) ?? []
            
            // Combine and sort matches
            var allMatches: [(range: NSRange, type: String)] = []
            for match in hashtagMatches {
                allMatches.append((match.range(at: 0), "hashtag"))
            }
            for match in mentionMatches {
                allMatches.append((match.range(at: 0), "mention"))
            }
            for match in urlMatches {
                allMatches.append((match.range, "url"))
            }
            allMatches.sort { $0.range.location < $1.range.location }
            
            // Build content with styling
            var lastEnd = content.startIndex
            let isMentioned = message.mentions?.contains(nickname) ?? false
            
            for (range, type) in allMatches {
                // Add text before match
                if let nsRange = Range(range, in: content) {
                    let beforeText = String(content[lastEnd..<nsRange.lowerBound])
                    if !beforeText.isEmpty {
                        var beforeStyle = AttributeContainer()
                        beforeStyle.foregroundColor = primaryColor
                        beforeStyle.font = .system(size: 14, design: .monospaced)
                        if isMentioned {
                            beforeStyle.font = beforeStyle.font?.bold()
                        }
                        result.append(AttributedString(beforeText).mergingAttributes(beforeStyle))
                    }
                    
                    // Add styled match
                    let matchText = String(content[nsRange])
                    var matchStyle = AttributeContainer()
                    matchStyle.font = .system(size: 14, weight: .semibold, design: .monospaced)
                    
                    if type == "hashtag" {
                        matchStyle.foregroundColor = Color.blue
                        matchStyle.underlineStyle = .single
                    } else if type == "mention" {
                        matchStyle.foregroundColor = Color.orange
                    } else if type == "url" {
                        matchStyle.foregroundColor = Color.blue
                        matchStyle.underlineStyle = .single
                    }
                    
                    result.append(AttributedString(matchText).mergingAttributes(matchStyle))
                    lastEnd = nsRange.upperBound
                }
            }
            
            // Add remaining text
            if lastEnd < content.endIndex {
                let remainingText = String(content[lastEnd...])
                var remainingStyle = AttributeContainer()
                remainingStyle.foregroundColor = primaryColor
                remainingStyle.font = .system(size: 14, design: .monospaced)
                if isMentioned {
                    remainingStyle.font = remainingStyle.font?.bold()
                }
                result.append(AttributedString(remainingText).mergingAttributes(remainingStyle))
            }
        } else {
            // System message
            var contentStyle = AttributeContainer()
            contentStyle.foregroundColor = Color.gray
            let content = AttributedString("* \(message.content) *")
            contentStyle.font = .system(size: 12, design: .monospaced).italic()
            result.append(content.mergingAttributes(contentStyle))
        }
        
        // Cache the formatted text
        message.setCachedFormattedText(result, isDark: isDark)
        
        return result
    }
    
    func formatMessage(_ message: BitchatMessage, colorScheme: ColorScheme) -> AttributedString {
        var result = AttributedString()
        
        let isDark = colorScheme == .dark
        let primaryColor = isDark ? Color.green : Color(red: 0, green: 0.5, blue: 0)
        let secondaryColor = primaryColor.opacity(0.7)
        
        let timestamp = AttributedString("[\(formatTimestamp(message.timestamp))] ")
        var timestampStyle = AttributeContainer()
        timestampStyle.foregroundColor = message.sender == "system" ? Color.gray : secondaryColor
        timestampStyle.font = .system(size: 12, design: .monospaced)
        result.append(timestamp.mergingAttributes(timestampStyle))
        
        if message.sender == "system" {
            let content = AttributedString("* \(message.content) *")
            var contentStyle = AttributeContainer()
            contentStyle.foregroundColor = Color.gray
            contentStyle.font = .system(size: 12, design: .monospaced).italic()
            result.append(content.mergingAttributes(contentStyle))
        } else {
            let sender = AttributedString("<\(message.sender)> ")
            var senderStyle = AttributeContainer()
            
            // Use consistent color for all senders
            senderStyle.foregroundColor = primaryColor
            // Bold the user's own nickname
            let fontWeight: Font.Weight = message.sender == nickname ? .bold : .medium
            senderStyle.font = .system(size: 12, weight: fontWeight, design: .monospaced)
            result.append(sender.mergingAttributes(senderStyle))
            
            
            // Process content to highlight mentions
            let contentText = message.content
            var processedContent = AttributedString()
            
            // Regular expression to find @mentions
            let pattern = "@([a-zA-Z0-9_]+)"
            let regex = try? NSRegularExpression(pattern: pattern, options: [])
            let matches = regex?.matches(in: contentText, options: [], range: NSRange(location: 0, length: contentText.count)) ?? []
            
            var lastEndIndex = contentText.startIndex
            
            for match in matches {
                // Add text before the mention
                if let range = Range(match.range(at: 0), in: contentText) {
                    let beforeText = String(contentText[lastEndIndex..<range.lowerBound])
                    if !beforeText.isEmpty {
                        var normalStyle = AttributeContainer()
                        normalStyle.font = .system(size: 14, design: .monospaced)
                        normalStyle.foregroundColor = isDark ? Color.white : Color.black
                        processedContent.append(AttributedString(beforeText).mergingAttributes(normalStyle))
                    }
                    
                    // Add the mention with highlight
                    let mentionText = String(contentText[range])
                    var mentionStyle = AttributeContainer()
                    mentionStyle.font = .system(size: 14, weight: .semibold, design: .monospaced)
                    mentionStyle.foregroundColor = Color.orange
                    processedContent.append(AttributedString(mentionText).mergingAttributes(mentionStyle))
                    
                    lastEndIndex = range.upperBound
                }
            }
            
            // Add any remaining text
            if lastEndIndex < contentText.endIndex {
                let remainingText = String(contentText[lastEndIndex...])
                var normalStyle = AttributeContainer()
                normalStyle.font = .system(size: 14, design: .monospaced)
                normalStyle.foregroundColor = isDark ? Color.white : Color.black
                processedContent.append(AttributedString(remainingText).mergingAttributes(normalStyle))
            }
            
            result.append(processedContent)
            
            if message.isRelay, let originalSender = message.originalSender {
                let relay = AttributedString(" (via \(originalSender))")
                var relayStyle = AttributeContainer()
                relayStyle.foregroundColor = secondaryColor
                relayStyle.font = .system(size: 11, design: .monospaced)
                result.append(relay.mergingAttributes(relayStyle))
            }
        }
        
        return result
    }
    
    // MARK: - Noise Protocol Support
    
    func updateEncryptionStatusForPeers() {
        for peerID in connectedPeers {
            updateEncryptionStatusForPeer(peerID)
        }
    }
    
    func updateEncryptionStatusForPeer(_ peerID: String) {
        let noiseService = meshService.getNoiseService()
        
        if noiseService.hasEstablishedSession(with: peerID) {
            // Check if fingerprint is verified using our persisted data
            if let fingerprint = getFingerprint(for: peerID),
               verifiedFingerprints.contains(fingerprint) {
                peerEncryptionStatus[peerID] = .noiseVerified
            } else {
                peerEncryptionStatus[peerID] = .noiseSecured
            }
        } else if noiseService.hasSession(with: peerID) {
            // Session exists but not established - handshaking
            peerEncryptionStatus[peerID] = .noiseHandshaking
        } else {
            // No session at all
            peerEncryptionStatus[peerID] = Optional.none
        }
        
        // Invalidate cache when encryption status changes
        invalidateEncryptionCache(for: peerID)
        
        // Force UI update
        DispatchQueue.main.async { [weak self] in
            self?.objectWillChange.send()
        }
    }
    
    func getEncryptionStatus(for peerID: String) -> EncryptionStatus {
        // Check cache first
        if let cachedStatus = encryptionStatusCache[peerID] {
            return cachedStatus
        }
        
        // This must be a pure function - no state mutations allowed
        // to avoid SwiftUI update loops
        
        let sessionState = meshService.getNoiseSessionState(for: peerID)
        let storedStatus = peerEncryptionStatus[peerID]
        
        let status: EncryptionStatus
        
        // Determine status based on session state
        switch sessionState {
        case .established:
            // We have encryption, now check if it's verified
            if let fingerprint = getFingerprint(for: peerID) {
                if verifiedFingerprints.contains(fingerprint) {
                    status = .noiseVerified
                } else {
                    status = .noiseSecured
                }
            } else {
                // We have a session but no fingerprint yet - still secured
                status = .noiseSecured
            }
        case .handshaking, .handshakeQueued:
            // Currently establishing encryption
            status = .noiseHandshaking
        case .none:
            // No handshake attempted
            status = .noHandshake
        case .failed:
            // Handshake failed - show broken lock
            status = .none
        }
        
        // Cache the result
        encryptionStatusCache[peerID] = status
        
        // Only log occasionally to avoid spam
        if Int.random(in: 0..<100) == 0 {
            SecureLogger.log("getEncryptionStatus for \(peerID): sessionState=\(sessionState), stored=\(String(describing: storedStatus)), final=\(status)", category: SecureLogger.security, level: .debug)
        }
        
        return status
    }
    
    // Clear caches when data changes
    private func invalidateEncryptionCache(for peerID: String? = nil) {
        if let peerID = peerID {
            encryptionStatusCache.removeValue(forKey: peerID)
        } else {
            encryptionStatusCache.removeAll()
        }
    }
    
    private func invalidateRSSIColorCache() {
        rssiColorCache.removeAll()
    }
    
    // MARK: - Message Batching
    
    private func trimMessagesIfNeeded() {
        if messages.count > maxMessages {
            let removeCount = messages.count - maxMessages
            messages.removeFirst(removeCount)
        }
    }
    
    private func trimPrivateChatMessagesIfNeeded(for peerID: String) {
        if let count = privateChats[peerID]?.count, count > maxMessages {
            let removeCount = count - maxMessages
            privateChats[peerID]?.removeFirst(removeCount)
        }
    }
    
    private func addMessageToBatch(_ message: BitchatMessage) {
        print("ChatViewModel: Adding message to batch from \(message.sender): \(message.content)")
        pendingMessages.append(message)
        scheduleBatchFlush()
    }
    
    private func addPrivateMessageToBatch(_ message: BitchatMessage, for peerID: String) {
        if pendingPrivateMessages[peerID] == nil {
            pendingPrivateMessages[peerID] = []
        }
        pendingPrivateMessages[peerID]?.append(message)
        scheduleBatchFlush()
    }
    
    private func scheduleBatchFlush() {
        // Cancel existing timer
        messageBatchTimer?.invalidate()
        
        // Schedule new flush
        messageBatchTimer = Timer.scheduledTimer(withTimeInterval: messageBatchInterval, repeats: false) { [weak self] _ in
            self?.flushMessageBatch()
        }
    }
    
    private func flushMessageBatch() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            
            // Process pending public messages
            if !self.pendingMessages.isEmpty {
                let messagesToAdd = self.pendingMessages
                self.pendingMessages.removeAll()
                
                // Add all messages at once
                self.messages.append(contentsOf: messagesToAdd)
                
                // Sort once after batch addition
                self.messages.sort { $0.timestamp < $1.timestamp }
                
                // Trim once if needed
                self.trimMessagesIfNeeded()
            }
            
            // Process pending private messages
            if !self.pendingPrivateMessages.isEmpty {
                let privateMessageBatches = self.pendingPrivateMessages
                self.pendingPrivateMessages.removeAll()
                
                for (peerID, messagesToAdd) in privateMessageBatches {
                    if self.privateChats[peerID] == nil {
                        self.privateChats[peerID] = []
                    }
                    
                    // Add all messages for this peer at once
                    self.privateChats[peerID]?.append(contentsOf: messagesToAdd)
                    
                    // Sort once after batch addition
                    self.privateChats[peerID]?.sort { $0.timestamp < $1.timestamp }
                    
                    // Trim once if needed
                    self.trimPrivateChatMessagesIfNeeded(for: peerID)
                }
            }
            
            // Single UI update for all changes
            self.objectWillChange.send()
        }
    }
    
    // Force immediate flush for high-priority messages
    private func flushMessageBatchImmediately() {
        messageBatchTimer?.invalidate()
        flushMessageBatch()
    }
    
    // Update encryption status in appropriate places, not during view updates
    private func updateEncryptionStatus(for peerID: String) {
        let noiseService = meshService.getNoiseService()
        
        if noiseService.hasEstablishedSession(with: peerID) {
            if let fingerprint = getFingerprint(for: peerID) {
                if verifiedFingerprints.contains(fingerprint) {
                    peerEncryptionStatus[peerID] = .noiseVerified
                } else {
                    peerEncryptionStatus[peerID] = .noiseSecured
                }
            } else {
                // Session established but no fingerprint yet
                peerEncryptionStatus[peerID] = .noiseSecured
            }
        } else if noiseService.hasSession(with: peerID) {
            peerEncryptionStatus[peerID] = .noiseHandshaking
        } else {
            peerEncryptionStatus[peerID] = Optional.none
        }
        
        // Invalidate cache when encryption status changes
        invalidateEncryptionCache(for: peerID)
        
        // Trigger UI update
        DispatchQueue.main.async { [weak self] in
            self?.objectWillChange.send()
        }
    }
    
    // MARK: - Fingerprint Management
    
    func showFingerprint(for peerID: String) {
        showingFingerprintFor = peerID
    }
    
    func getFingerprint(for peerID: String) -> String? {
        // Remove debug logging to prevent console spam during view updates
        
        // First try to get fingerprint from mesh service's peer ID rotation mapping
        if let fingerprint = meshService.getFingerprint(for: peerID) {
            return fingerprint
        }
        
        // Fallback to noise service (direct Noise session fingerprint)
        if let fingerprint = meshService.getNoiseService().getPeerFingerprint(peerID) {
            return fingerprint
        }
        
        // Last resort: check local mapping
        if let fingerprint = peerIDToPublicKeyFingerprint[peerID] {
            return fingerprint
        }
        
        return nil
    }
    
    // Helper to resolve nickname for a peer ID through various sources
    func resolveNickname(for peerID: String) -> String {
        // Guard against empty or very short peer IDs
        guard !peerID.isEmpty else {
            return "unknown"
        }
        
        // Check if this might already be a nickname (not a hex peer ID)
        // Peer IDs are hex strings, so they only contain 0-9 and a-f
        let isHexID = peerID.allSatisfy { $0.isHexDigit }
        if !isHexID {
            // If it's already a nickname, just return it
            return peerID
        }
        
        // First try direct peer nicknames from mesh service
        let peerNicknames = meshService.getPeerNicknames()
        if let nickname = peerNicknames[peerID] {
            return nickname
        }
        
        // Try to resolve through fingerprint and social identity
        if let fingerprint = getFingerprint(for: peerID) {
            if let identity = SecureIdentityStateManager.shared.getSocialIdentity(for: fingerprint) {
                // Prefer local petname if set
                if let petname = identity.localPetname {
                    return petname
                }
                // Otherwise use their claimed nickname
                return identity.claimedNickname
            }
        }
        
        // Fallback to anonymous with shortened peer ID
        // Ensure we have at least 4 characters for the prefix
        let prefixLength = min(4, peerID.count)
        let prefix = String(peerID.prefix(prefixLength))
        
        // Avoid "anonanon" by checking if ID already starts with "anon"
        if prefix.starts(with: "anon") {
            return "peer\(prefix)"
        }
        return "anon\(prefix)"
    }
    
    func getMyFingerprint() -> String {
        let fingerprint = meshService.getNoiseService().getIdentityFingerprint()
        return fingerprint
    }
    
    func verifyFingerprint(for peerID: String) {
        guard let fingerprint = getFingerprint(for: peerID) else { return }
        
        // Update secure storage with verified status
        SecureIdentityStateManager.shared.setVerified(fingerprint: fingerprint, verified: true)
        
        // Update local set for UI
        verifiedFingerprints.insert(fingerprint)
        
        // Update encryption status after verification
        updateEncryptionStatus(for: peerID)
    }
    
    func loadVerifiedFingerprints() {
        // Load verified fingerprints directly from secure storage
        verifiedFingerprints = SecureIdentityStateManager.shared.getVerifiedFingerprints()
    }
    
    private func setupNoiseCallbacks() {
        let noiseService = meshService.getNoiseService()
        
        // Set up authentication callback
        noiseService.onPeerAuthenticated = { [weak self] peerID, fingerprint in
            DispatchQueue.main.async {
                guard let self = self else { return }
                
                SecureLogger.log("ChatViewModel: Peer authenticated - \(peerID), fingerprint: \(fingerprint)", category: SecureLogger.security, level: .info)
                
                // Update encryption status
                if self.verifiedFingerprints.contains(fingerprint) {
                    self.peerEncryptionStatus[peerID] = .noiseVerified
                    SecureLogger.log("ChatViewModel: Setting encryption status to noiseVerified for \(peerID)", category: SecureLogger.security, level: .info)
                } else {
                    self.peerEncryptionStatus[peerID] = .noiseSecured
                    SecureLogger.log("ChatViewModel: Setting encryption status to noiseSecured for \(peerID)", category: SecureLogger.security, level: .info)
                }
                
                // Invalidate cache when encryption status changes
                self.invalidateEncryptionCache(for: peerID)
                
                // Force UI update
                self.objectWillChange.send()
            }
        }
        
        // Set up handshake required callback
        noiseService.onHandshakeRequired = { [weak self] peerID in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.peerEncryptionStatus[peerID] = .noiseHandshaking
                
                // Invalidate cache when encryption status changes
                self.invalidateEncryptionCache(for: peerID)
                
                // Force UI update
                self.objectWillChange.send()
            }
        }
    }
    
    // MARK: - Haptic Feedback
    
    private func triggerMentionHaptic(for message: BitchatMessage) {
        #if os(iOS)
        let impactFeedback = UIImpactFeedbackGenerator(style: .heavy)
        impactFeedback.prepare()
        impactFeedback.impactOccurred()
        
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            impactFeedback.impactOccurred()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            impactFeedback.impactOccurred()
        }
        #endif
    }
    
    // MARK: - Message Delivery Status
    
    private func updateMessageDeliveryStatus(_ messageID: String, status: DeliveryStatus) {
        // Update delivery status in messages
        if let index = messages.firstIndex(where: { $0.id == messageID }) {
            messages[index].deliveryStatus = status
        }
        
        // Update in private chats
        for (peerID, chatMessages) in privateChats {
            if let index = chatMessages.firstIndex(where: { $0.id == messageID }) {
                var updatedMessages = chatMessages
                updatedMessages[index].deliveryStatus = status
                privateChats[peerID] = updatedMessages
            }
        }
        
        // Force UI update
        objectWillChange.send()
    }
    
    // MARK: - Gateway Management
    
    private func parseGatewayStatus(_ statusMessage: String) -> GatewayStatusInfo? {
        // Parse STATUS|gateway_id|gateway_name|connected_count|nicknames
        let trimmedMessage = statusMessage.trimmingCharacters(in: .whitespaces)
        let parts = trimmedMessage.split(separator: "|")
        guard parts.count >= 3, parts[0] == "STATUS" else { return nil }
        
        let gatewayId = String(parts[1])
        let gatewayName = String(parts[2])
        
        // Handle both formats: with and without connected count
        let connectedCount: Int
        var nicknames: [String] = []
        
        if parts.count >= 4 {
            // Has connected count
            connectedCount = Int(parts[3]) ?? 0
            if parts.count > 4 {
                nicknames = String(parts[4]).split(separator: ",").map { String($0) }
            }
        } else {
            // No connected count - assume 0
            connectedCount = 0
        }
        
        return GatewayStatusInfo(
            gatewayId: gatewayId,
            gatewayName: gatewayName,
            connectedCount: connectedCount,
            nicknames: nicknames
        )
    }
    
    @MainActor
    private func updateGatewayPeers(from statusInfo: GatewayStatusInfo) {
        // Update the gateway transport's status dictionary so it knows about all gateways
        if let transport = gatewayTransport {
            let previousStatus = transport.gatewayStatuses[statusInfo.gatewayId]
            transport.gatewayStatuses[statusInfo.gatewayId] = statusInfo
            
            // Only log if this is a new gateway or the count changed
            if previousStatus == nil {
                print("ChatViewModel: New gateway discovered: \(statusInfo.gatewayName) (ID: \(statusInfo.gatewayId)) - \(statusInfo.connectedCount) devices")
            } else if previousStatus?.connectedCount != statusInfo.connectedCount {
                print("ChatViewModel: Gateway \(statusInfo.gatewayName) device count changed: \(previousStatus?.connectedCount ?? 0) -> \(statusInfo.connectedCount)")
            }
        }
        
        // Remove old peers from this gateway
        gatewayPeers.removeAll { $0.gatewayId == statusInfo.gatewayId }
        
        // Add new peers
        let timestamp = Date()
        for nickname in statusInfo.nicknames {
            let peer = GatewayPeer(
                id: "\(statusInfo.gatewayId)_\(nickname)",
                nickname: nickname,
                gatewayId: statusInfo.gatewayId,
                gatewayName: statusInfo.gatewayName,
                rssi: nil, // Could be added to status message later
                timestamp: timestamp
            )
            gatewayPeers.append(peer)
        }
        
        // Remove stale gateway peers (older than 30 seconds)
        let cutoffTime = Date().addingTimeInterval(-30)
        gatewayPeers.removeAll { $0.timestamp < cutoffTime }
        
        // Sort by gateway name and nickname
        gatewayPeers.sort { 
            if $0.gatewayName == $1.gatewayName {
                return $0.nickname < $1.nickname
            }
            return $0.gatewayName < $1.gatewayName
        }
    }
    
    // MARK: - Public Methods
    
    // ... existing code ...
}

// MARK: - BitchatDelegate

extension ChatViewModel: BitchatDelegate {
    func didReceiveMessage(_ message: BitchatMessage) {
        // Check if message is from a blocked user
        if isPeerBlocked(message.senderPeerID ?? "") {
            return
        }
        
        // Mark message as received via gateway if we have no direct peers but are gateway connected
        if connectedPeers.isEmpty && isGatewayConnected {
            message.sentViaGateway = true
        }
        
        // Handle private messages
        if message.isPrivate {
            guard let senderPeerID = message.senderPeerID else { return }
            
            // Initialize chat if needed
            if privateChats[senderPeerID] == nil {
                privateChats[senderPeerID] = []
            }
            
            // Add to private chat
            addPrivateMessageToBatch(message, for: senderPeerID)
            
            // Mark as unread if not in active chat
            if selectedPrivateChatPeer != senderPeerID {
                unreadPrivateMessages.insert(senderPeerID)
            }
            
            // Send notification
            let senderNickname = resolveNickname(for: senderPeerID)
            NotificationService.shared.sendLocalNotification(
                title: "Private message from \(senderNickname)",
                body: message.content,
                identifier: message.id,
                userInfo: ["peerID": senderPeerID]
            )
        } else {
            // Add to public messages
            addMessageToBatch(message)
        }
        
        // Handle mentions
        if let mentions = message.mentions, mentions.contains(nickname) {
            triggerMentionHaptic(for: message)
        }
    }
    
    func didConnectToPeer(_ peerID: String) {
        // Update connected peers list
        if !connectedPeers.contains(peerID) {
            connectedPeers.append(peerID)
        }
        isConnected = !connectedPeers.isEmpty
        
        // Add system message
        let displayName = resolveNickname(for: peerID)
        let systemMessage = BitchatMessage(
            sender: "system",
            content: "\(displayName) connected",
            timestamp: Date(),
            isRelay: false,
            originalSender: nil
        )
        addMessageToBatch(systemMessage)
    }
    
    func didDisconnectFromPeer(_ peerID: String) {
        // Remove from connected peers
        connectedPeers.removeAll { $0 == peerID }
        isConnected = !connectedPeers.isEmpty
        
        // Remove ephemeral session
        SecureIdentityStateManager.shared.removeEphemeralSession(peerID: peerID)
        
        // Clear sent read receipts for this peer
        if let messages = privateChats[peerID] {
            for message in messages {
                if message.senderPeerID == peerID {
                    sentReadReceipts.remove(message.id)
                }
            }
        }
        
        // Add system message
        let displayName = resolveNickname(for: peerID)
        let systemMessage = BitchatMessage(
            sender: "system",
            content: "\(displayName) disconnected",
            timestamp: Date(),
            isRelay: false,
            originalSender: nil
        )
        addMessageToBatch(systemMessage)
    }
    
    func didUpdatePeerList(_ peers: [String]) {
        connectedPeers = peers
        isConnected = !peers.isEmpty
        
        // Update encryption status for all peers
        updateEncryptionStatusForPeers()
        
        // Invalidate nickname cache
        cachedNicknames.removeAll()
        lastNicknameUpdate = .distantPast
    }
    
    func isFavorite(fingerprint: String) -> Bool {
        return SecureIdentityStateManager.shared.isFavorite(fingerprint: fingerprint)
    }
    
    func didReceiveDeliveryAck(_ ack: DeliveryAck) {
        // Process the delivery acknowledgment
        DeliveryTracker.shared.processDeliveryAck(ack)
        
        // Update local message status
        updateMessageDeliveryStatus(ack.originalMessageID, status: .delivered(to: ack.recipientNickname, at: Date()))
    }
    
    func didReceiveReadReceipt(_ receipt: ReadReceipt) {
        // Update delivery status to read
        updateMessageDeliveryStatus(receipt.originalMessageID, status: .read(by: receipt.readerNickname, at: Date()))
    }
    
    func didUpdateMessageDeliveryStatus(_ messageID: String, status: DeliveryStatus) {
        updateMessageDeliveryStatus(messageID, status: status)
    }
    
    func peerAvailabilityChanged(_ peerID: String, available: Bool) {
        // Handle peer availability changes
        // For now, just log it - can be extended later
        if !available {
            print("Peer \(peerID) became unavailable")
        }
    }
}
