import Foundation

struct GatewayStats: Codable {
    let uptime: UInt32
    let loraRx: UInt32
    let loraTx: UInt32
    let bleRx: UInt32
    let bleTx: UInt32
    let dedupHits: UInt32
    let crcErrors: UInt32
    let dutyBlockMs: UInt32
    
    init(uptime: UInt32, loraRx: UInt32, loraTx: UInt32, bleRx: UInt32, bleTx: UInt32, dedupHits: UInt32, crcErrors: UInt32, dutyBlockMs: UInt32) {
        self.uptime = uptime
        self.loraRx = loraRx
        self.loraTx = loraTx
        self.bleRx = bleRx
        self.bleTx = bleTx
        self.dedupHits = dedupHits
        self.crcErrors = crcErrors
        self.dutyBlockMs = dutyBlockMs
    }
    
    static func fromData(_ data: Data) -> GatewayStats? {
        guard data.count >= 32 else { return nil }
        
        return data.withUnsafeBytes { bytes in
            let buffer = bytes.bindMemory(to: UInt32.self)
            
            return GatewayStats(
                uptime: UInt32(littleEndian: buffer[0]),
                loraRx: UInt32(littleEndian: buffer[1]),
                loraTx: UInt32(littleEndian: buffer[2]),
                bleRx: UInt32(littleEndian: buffer[3]),
                bleTx: UInt32(littleEndian: buffer[4]),
                dedupHits: UInt32(littleEndian: buffer[5]),
                crcErrors: UInt32(littleEndian: buffer[6]),
                dutyBlockMs: UInt32(littleEndian: buffer[7])
            )
        }
    }
}

struct GatewayConfig: Codable {
    let gwId: UInt32
    let region: UInt8
    let sf: UInt8
    let bw: UInt8
    let txDbm: Int8
    let ttl: UInt8
    let bleAdvIntMs: UInt8
    let logLevel: UInt8
    
    init(gwId: UInt32, region: UInt8, sf: UInt8, bw: UInt8, txDbm: Int8, ttl: UInt8, bleAdvIntMs: UInt8, logLevel: UInt8) {
        self.gwId = gwId
        self.region = region
        self.sf = sf
        self.bw = bw
        self.txDbm = txDbm
        self.ttl = ttl
        self.bleAdvIntMs = bleAdvIntMs
        self.logLevel = logLevel
    }
    
    static func fromData(_ data: Data) -> GatewayConfig? {
        guard data.count >= 24 else { return nil } // Config struct size without secret
        
        return data.withUnsafeBytes { bytes in
            let buffer = bytes.bindMemory(to: UInt8.self)
            
            let gwId = UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 0, as: UInt32.self))
            let region = buffer[4]
            let sf = buffer[5]
            let bw = buffer[6]
            let txDbm = Int8(bitPattern: buffer[7])
            let ttl = buffer[8]
            let bleAdvIntMs = buffer[9]
            let logLevel = buffer[10]
            
            return GatewayConfig(
                gwId: gwId,
                region: region,
                sf: sf,
                bw: bw,
                txDbm: txDbm,
                ttl: ttl,
                bleAdvIntMs: bleAdvIntMs,
                logLevel: logLevel
            )
        }
    }
}