import BroFoundation

/// What one makemkvcon run said, fed one event at a time: the saved / failed counts, the errors and the first of
/// them (usually the cause), MakeMKV's space warning, read errors, a renumbered drive, the debug log, notices and
/// the version. When `stopReason` is set, the caller stops the process.
public struct RunAccumulator: Sendable {
    private let readsData: Bool
    private let expectedIndex: Int?
    private let expectedDevice: String

    /// Titles saved, from 5036 / 5005 or the summary 5037 / 5004.
    public private(set) var saved: Int?
    /// Titles failed, from the summary 5037 / 5004.
    public private(set) var failed: Int?
    /// Every message of severity error, in order.
    public private(set) var errors: [RobotMessage] = []
    /// The first error other than the saved / failed summary (5037, 5004): usually the cause.
    public private(set) var firstError: RobotMessage?
    /// MakeMKV's warning that the output may not fit (5038).
    public private(set) var spaceWarning: RobotMessage?
    /// Errors while reading the disc's data (only when the run reads data).
    public private(set) var readErrors: [RobotMessage] = []
    /// The drive index now names another device (drive.renumbered).
    public private(set) var driveMismatch: BroMessage?
    /// MakeMKV's debug log (app_ShowDebug), named by message 1004, as a path.
    public private(set) var debugLog: String?
    /// The LibreDrive detail of message 1011, when the drive uses LibreDrive.
    public private(set) var libreDrive: String?
    /// The first notice other than LibreDrive: a key, version or drive problem.
    public private(set) var problem: MakemkvNotice?
    /// MakeMKV's version (message 1005).
    public private(set) var makemkvVersion: String?
    /// Why the run must stop now (space.makemkvWarning or drive.renumbered); nil while it may go on.
    public private(set) var stopReason: BroMessage?

    /// - Parameters:
    ///   - readsData: the run reads the disc's data (a rip or backup): its errors count as read errors.
    ///   - expectedIndex: the MakeMKV drive index the run uses, with its device, to catch renumbered drives.
    public init(readsData: Bool = false, expectedIndex: Int? = nil, expectedDevice: String = "") {
        self.readsData = readsData
        self.expectedIndex = expectedIndex
        self.expectedDevice = expectedDevice
    }

    public mutating func feed(_ event: RobotEvent) {
        switch event {
        case .message(let m):
            feedMessage(m)
        case let .drive(index, _, _, _, _, device):
            if let expected = expectedIndex, index == expected, !expectedDevice.isEmpty, !device.isEmpty,
               MessageCatalog.asciiLower(device) != MessageCatalog.asciiLower(expectedDevice) {
                if driveMismatch == nil {
                    driveMismatch = BroMessage(.driveRenumbered, [("index", .integer(Int64(expected))), ("now", .string(device)),
                                                                  ("expected", .string(expectedDevice))], severity: .error)
                }
                if stopReason == nil { stopReason = driveMismatch }
            }
        default:
            break
        }
    }

    private mutating func feedMessage(_ m: RobotMessage) {
        if let notice = MessageCatalog.notice(m) {
            if case .libreDrive(let detail) = notice {
                if libreDrive == nil { libreDrive = detail }
            } else if problem == nil {
                problem = notice
            }
        }
        // "Debug logging enabled, log will be saved as file:///C:/…/MakeMKV_log.txt"
        if m.code == 1004, let first = m.params.first, let path = RunAccumulator.filePath(first) { debugLog = path }
        if MessageCatalog.severity(m) == .error {
            if firstError == nil && m.code != 5037 && m.code != 5004 { firstError = m }
            errors.append(m)
            if readsData { readErrors.append(m) }
        }
        if m.code == 1005 && makemkvVersion == nil { makemkvVersion = m.params.first ?? m.text }
        if m.code == 5036 || m.code == 5005, let first = m.params.first, let n = Robot.int(first) { saved = n }
        if m.code == 5038 && spaceWarning == nil {
            // "The total size of all output files may reach as much as … while there are only … free": stop before
            // anything is written rather than fail when the disk fills up.
            spaceWarning = m
            if stopReason == nil { stopReason = BroMessage(.spaceMakemkvWarning, [("text", .string(m.text))], severity: .error) }
        }
        if m.code == 5037 || m.code == 5004, m.params.count > 1 {
            if let s = Robot.int(m.params[0]) { saved = s }
            if let f = Robot.int(m.params[1]) { failed = f }
        }
    }

    /// The path of a file:// URL (percent-decoded; `file:///C:/x` → `C:/x`), else nil.
    static func filePath(_ url: String) -> String? {
        guard MessageCatalog.asciiLower(String(url.prefix(7))) == "file://" else { return nil }
        var rest = Array(url.utf8.dropFirst(7))
        if MessageCatalog.asciiLower(String(decoding: rest.prefix(10), as: UTF8.self)) == "localhost/" { rest.removeFirst(9) }
        guard rest.first == UInt8(ascii: "/") else { return nil }
        if rest.count >= 3, (rest[1] | 0x20) >= UInt8(ascii: "a"), (rest[1] | 0x20) <= UInt8(ascii: "z"), rest[2] == UInt8(ascii: ":") {
            rest.removeFirst()
        }
        func hex(_ c: UInt8) -> UInt8? {
            switch c {
            case 0x30...0x39: return c - 0x30
            case 0x41...0x46: return c - 0x41 + 10
            case 0x61...0x66: return c - 0x61 + 10
            default: return nil
            }
        }
        var bytes: [UInt8] = []
        var i = 0
        while i < rest.count {
            if rest[i] == UInt8(ascii: "%"), i + 2 < rest.count, let h = hex(rest[i + 1]), let l = hex(rest[i + 2]) {
                bytes.append(h * 16 + l)
                i += 3
            } else {
                bytes.append(rest[i])
                i += 1
            }
        }
        return String(decoding: bytes, as: UTF8.self)
    }
}
