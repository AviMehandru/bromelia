import BroFoundation

/// Whether a backup looks like a disc: a folder with a BDMV, VIDEO_TS or HVDVD_TS structure, or an ISO image (an
/// ISO 9660 or UDF volume descriptor at byte 32769).
public enum BackupStructure {
    /// What is wrong with the backup called `name`, or nil. `entries` lists the folder's files as relative paths (a
    /// trailing / is an empty folder; nil: not a folder); `isoHeader` is the file's bytes from 32769 (at most 5;
    /// nil: not a file).
    public static func problem(_ name: String, iso: Bool, entries: [String]?, isoHeader: [UInt8]?) -> BroMessage? {
        func named(_ code: MessageCode) -> BroMessage { BroMessage(code, [("name", .string(name))]) }
        if iso {
            if entries != nil { return named(.structureIsFolder) }
            guard let isoHeader else { return named(.structureNotCreated) }
            let id = isoHeader.count >= 5 ? String(decoding: isoHeader.prefix(5), as: UTF8.self) : ""
            return id == "CD001" || id == "BEA01" ? nil : named(.structureNotImage)
        }
        guard let entries else { return named(isoHeader != nil ? .structureNotFolder : .structureNotCreated) }
        func has(_ prefix: String) -> Bool { entries.contains { MessageCatalog.asciiLower($0).hasPrefix(MessageCatalog.asciiLower(prefix)) } }
        if has("BDMV/") { return has("BDMV/index.bdmv") ? nil : BroMessage(.structureBdmvIndexMissing) }
        if has("VIDEO_TS/") { return has("VIDEO_TS/VIDEO_TS.IFO") ? nil : BroMessage(.structureVideoTsIfoMissing) }
        if has("HVDVD_TS/") { return nil }
        return BroMessage(.structureNoDiscFolder)
    }
}
