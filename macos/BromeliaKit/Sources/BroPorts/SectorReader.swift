import BroDomain
import BroFoundation

/// Raw reads of a disc (data discs, rescue).
public protocol SectorReader: Sendable {
    /// count 2048-byte sectors from sector; fails on a read error.
    func read(_ sector: Int64, count: Int) throws(BroError) -> [UInt8]

    func sectorCount() throws(BroError) -> Int64

    func close()
}
