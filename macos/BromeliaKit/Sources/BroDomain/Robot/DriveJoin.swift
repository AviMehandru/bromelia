import CryptoKit

/// MakeMKV's drives ⇄ the OS's drives and the configuration's entries.
public enum DriveJoin {
    /// One entry per present MakeMKV drive, with the OS drive of the same device; then the OS drives MakeMKV
    /// didn't report (their id comes from the device until MakeMKV names them).
    public static func join(_ makemkvDrives: [MakemkvDrive], osDrives: [OsDrive]) -> [JoinedDrive] {
        var joined: [JoinedDrive] = []
        var used = Set<Int>()
        for m in makemkvDrives where MakemkvDrive.isPresent(m) {
            var os: OsDrive?
            if !m.device.isEmpty, let i = osDrives.indices.first(where: { !used.contains($0) && deviceKey(osDrives[$0].device) == deviceKey(m.device) }) {
                used.insert(i)
                os = osDrives[i]
            }
            joined.append(JoinedDrive(driveId: driveId(m.identification, device: m.device), makemkv: m, os: os))
        }
        for (i, o) in osDrives.enumerated() where !used.contains(i) {
            joined.append(JoinedDrive(driveId: driveId("", device: o.device), makemkv: nil, os: o))
        }
        return joined
    }

    /// `drv-` and the first 16 hex digits of the SHA-256 of the normalised identification (lower case, runs of
    /// spaces collapsed), or of `dev:<device>` when it's empty.
    public static func driveId(_ identification: String, device: String) -> String {
        let name = normalize(identification)
        let key = name.isEmpty ? "dev:" + device : name
        let digest = SHA256.hash(data: Array(key.utf8))
        return "drv-" + digest.prefix(8).map { b in
            let h = String(b, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
    }

    /// driveName compared case- and space-insensitively with the identification; devicePath (ignoring case)
    /// when driveName is empty.
    public static func matches(_ match: DriveMatch, drive: MakemkvDrive) -> Bool {
        let name = trimSpaces(match.driveName)
        if !name.isEmpty { return normalize(name) == normalize(drive.identification) }
        return !match.devicePath.isEmpty && MessageCatalog.asciiLower(match.devicePath) == MessageCatalog.asciiLower(drive.device)
    }

    /// The first enabled entry that matches, else the first that matches.
    public static func entryFor(_ drives: [DriveEntry], drive: MakemkvDrive) -> DriveEntry? {
        drives.first { $0.enabled && matches($0.match, drive: drive) } ?? drives.first { matches($0.match, drive: drive) }
    }

    /// The model part of an identification: `BD-RE NEW DRIVE 3.00 SN` → `NEW DRIVE 3.00`.
    public static func shortModel(_ identification: String) -> String {
        let parts = identification.split(separator: " ", omittingEmptySubsequences: true)
        if parts.count > 2 { return parts.dropFirst().prefix(3).joined(separator: " ") }
        return identification.isEmpty ? "Drive" : identification
    }

    static func normalize(_ s: String) -> String {
        MessageCatalog.asciiLower(s.split(whereSeparator: { $0 == " " || $0 == "\t" }).joined(separator: " "))
    }

    static func trimSpaces(_ s: String) -> String {
        String(s.drop { $0 == " " || $0 == "\t" }.reversed().drop { $0 == " " || $0 == "\t" }.reversed())
    }

    // /dev/rdisk4 (MakeMKV on macOS) and /dev/disk4 (DiskArbitration) are one drive; E: and E:\ too.
    static func deviceKey(_ device: String) -> String {
        var d = MessageCatalog.asciiLower(device)
        while d.hasSuffix("\\") || d.hasSuffix("/") { d.removeLast() }
        return d.hasPrefix("/dev/rdisk") ? "/dev/disk" + d.dropFirst(10) : d
    }
}
