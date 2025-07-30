import Foundation

struct BridgeHeader: Codable {
    var magic: UInt16      // 0xBC77
    var ver: UInt8         // 0x01
    var ttl: UInt8
    var msgId: UInt64
    var gwId: UInt32
    var fragIdx: UInt8
    var fragTotal: UInt8
    var payloadLen: UInt16
    var hop: UInt16
    var crc16: UInt16
    
    init(magic: UInt16 = 0xBC77, ver: UInt8 = 1, ttl: UInt8, msgId: UInt64, gwId: UInt32, fragIdx: UInt8, fragTotal: UInt8, payloadLen: UInt16, hop: UInt16, crc16: UInt16 = 0) {
        self.magic = magic
        self.ver = ver
        self.ttl = ttl
        self.msgId = msgId
        self.gwId = gwId
        self.fragIdx = fragIdx
        self.fragTotal = fragTotal
        self.payloadLen = payloadLen
        self.hop = hop
        self.crc16 = crc16
    }
    
    func toData() -> Data {
        var data = Data(capacity: 24)
        withUnsafeBytes(of: magic.littleEndian) { data.append(contentsOf: $0) }
        data.append(ver)
        data.append(ttl)
        withUnsafeBytes(of: msgId.littleEndian) { data.append(contentsOf: $0) }
        withUnsafeBytes(of: gwId.littleEndian) { data.append(contentsOf: $0) }
        data.append(fragIdx)
        data.append(fragTotal)
        withUnsafeBytes(of: payloadLen.littleEndian) { data.append(contentsOf: $0) }
        withUnsafeBytes(of: hop.littleEndian) { data.append(contentsOf: $0) }
        withUnsafeBytes(of: crc16.littleEndian) { data.append(contentsOf: $0) }
        return data
    }
    
    static func fromData(_ data: Data) -> BridgeHeader? {
        guard data.count >= 24 else { return nil }
        
        return data.withUnsafeBytes { bytes in
            let buffer = bytes.bindMemory(to: UInt8.self)
            
            let magic = UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 0, as: UInt16.self))
            let ver = buffer[2]
            let ttl = buffer[3]
            let msgId = UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: 4, as: UInt64.self))
            let gwId = UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 12, as: UInt32.self))
            let fragIdx = buffer[16]
            let fragTotal = buffer[17]
            let payloadLen = UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 18, as: UInt16.self))
            let hop = UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 20, as: UInt16.self))
            let crc16 = UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 22, as: UInt16.self))
            
            return BridgeHeader(magic: magic, ver: ver, ttl: ttl, msgId: msgId, gwId: gwId, fragIdx: fragIdx, fragTotal: fragTotal, payloadLen: payloadLen, hop: hop, crc16: crc16)
        }
    }
}