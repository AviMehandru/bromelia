import BroDomain
import BroFoundation
import BroTestSupport
import Testing

/// An event as the fixtures write it.
func eventJson(_ e: RobotEvent?) -> JsonValue {
    switch e {
    case nil: return .null
    case .message(let m)?:
        return .object([("kind", .string("message")), ("code", .integer(Int64(m.code))), ("flags", .integer(Int64(m.flags))),
                        ("text", .string(m.text)), ("format", .string(m.format)), ("params", .array(m.params.map(JsonValue.string))),
                        ("severity", .string(MessageCatalog.severity(m).rawValue))])
    case let .progressValue(current, total, max)?:
        return .object([("kind", .string("progressValue")), ("current", .integer(Int64(current))), ("total", .integer(Int64(total))),
                        ("max", .integer(Int64(max)))])
    case let .progressTotal(code, id, name)?:
        return .object([("kind", .string("progressTotal")), ("code", .integer(Int64(code))), ("id", .integer(Int64(id))), ("name", .string(name))])
    case let .progressCurrent(code, id, name)?:
        return .object([("kind", .string("progressCurrent")), ("code", .integer(Int64(code))), ("id", .integer(Int64(id))), ("name", .string(name))])
    case .titleCount(let n)?: return .object([("kind", .string("titleCount")), ("count", .integer(Int64(n)))])
    case .raw(let text)?: return .object([("kind", .string("raw")), ("text", .string(text))])
    case let other?: return .object([("kind", .string("\(other)"))])
    }
}

func accumulate(_ fixture: String, readsData: Bool = false) throws -> RunAccumulator {
    var acc = RunAccumulator(readsData: readsData)
    for line in try Fixtures.text(fixture).split(separator: "\n", omittingEmptySubsequences: false) {
        if let e = Robot.parseLine(String(line)) { acc.feed(e) }
    }
    return acc
}

func noticeJson(_ n: MakemkvNotice?) -> JsonValue {
    guard let n else { return .null }
    var members: [(key: String, value: JsonValue)]
    switch n {
    case .libreDrive(let detail): members = [("kind", .string("libreDrive")), ("detail", .string(detail))]
    case .libreDriveRequired: members = [("kind", .string("libreDriveRequired"))]
    case .keyExpired: members = [("kind", .string("keyExpired"))]
    case .evaluationNotStarted: members = [("kind", .string("evaluationNotStarted"))]
    case .versionTooOld: members = [("kind", .string("versionTooOld"))]
    }
    members.append(("licenseProblem", .bool(MakemkvNotice.isLicenseProblem(n))))
    return .object(members)
}

func driveOf(_ d: JsonValue?) -> MakemkvDrive {
    MakemkvDrive(index: 0, state: .inserted, flags: DiscFlags(raw: 0), identification: d?["identification"]?.string ?? "",
                 label: "", device: d?["device"]?.string ?? "")
}

func matchOf(_ m: JsonValue?) -> DriveMatch {
    DriveMatch(driveName: m?["driveName"]?.string ?? "", devicePath: m?["devicePath"]?.string ?? "")
}

struct RobotTests {
    @Test func robotCases() throws {
        let failures = try Fixtures.runCases("domain/robot.cases.json") { _, given, expect in
            if let fields = given["fields"]?.string {
                try Fixtures.same(expect["fields"], .array(Robot.splitFields(fields).map(JsonValue.string)), "fields")
                return true
            }
            if let line = given["line"]?.string {
                let e = Robot.parseLine(line)
                if let ev = expect["event"] {
                    try Fixtures.same(ev, eventJson(e), "event")
                } else if let notice = expect["notice"] {
                    var n: MakemkvNotice?
                    if case .message(let m)? = e { n = MessageCatalog.notice(m) }
                    try Fixtures.same(notice, noticeJson(n), "notice")
                } else {
                    return false
                }
                return true
            }
            if let m = given["message"] {
                let msg = RobotMessage(code: Int(m["code"]?.int ?? 0), flags: Int(m["flags"]?.int ?? 0), text: m["text"]?.string ?? "")
                try Fixtures.same(expect["severity"]?.string, MessageCatalog.severity(msg).rawValue, "severity")
                return true
            }
            if let codes = given["codes"] {
                for kind in ["error", "warning"] {
                    for code in codes[kind]?.array ?? [] {
                        let msg = RobotMessage(code: Int(code.int ?? 0), flags: Int(given["flags"]?.int ?? 0), text: "x")
                        try Fixtures.same(kind, MessageCatalog.severity(msg).rawValue, "severity of \(code)")
                    }
                }
                return true
            }
            if let fixture = given["fixture"]?.string {
                let acc = try accumulate(fixture)
                if let saved = expect["lastSaved"] { try Fixtures.same(saved.int, acc.saved.map(Int64.init), "saved") }
                if let codes = expect["errorCodes"] {
                    try Fixtures.same(codes, .array(acc.errors.map { .integer(Int64($0.code)) }), "error codes")
                }
                if let params = expect["lastErrorParams"] {
                    try Fixtures.same(params, .array((acc.errors.last?.params ?? []).map(JsonValue.string)), "last error's params")
                }
                return true
            }
            if let list = given["fixtures"]?.array {
                for f in list.compactMap(\.string) {
                    let part = expect["firstErrorContains"]?[f]?.string ?? "?"
                    let first = try accumulate(f).firstError
                    try Fixtures.check(first?.text.contains(part) == true, "\(f): first error \(first?.text ?? "none") doesn't contain \(part)")
                }
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func driveScanFixture() throws {
        let golden = try Fixtures.json("domain/drive-scan.drives.expected.json")
        let drives = try Fixtures.text(golden["input"]?.string ?? "").split(separator: "\n").compactMap { Robot.parseLine(String($0)) }
            .compactMap(MakemkvDrive.from)
        let actual = JsonValue.array(drives.map { d in
            var flags: [JsonValue] = []
            if d.flags.dvdFiles { flags.append(.string("dvdFiles")) }
            if d.flags.hdDvdFiles { flags.append(.string("hdDvdFiles")) }
            if d.flags.blurayFiles { flags.append(.string("blurayFiles")) }
            if d.flags.aacsFiles { flags.append(.string("aacsFiles")) }
            if d.flags.bdsvmFiles { flags.append(.string("bdsvmFiles")) }
            return .object([("index", .integer(Int64(d.index))), ("state", .string(d.state.rawValue)), ("present", .bool(MakemkvDrive.isPresent(d))),
                            ("flags", .array(flags)), ("typeText", .string(DiscFlags.typeText(d.flags))),
                            ("identification", .string(d.identification)), ("label", .string(d.label)), ("device", .string(d.device))])
        })
        #expect(golden["expect"] == actual)
    }

    @Test func driveJoinCases() throws {
        var failures = try Fixtures.runCases("domain/rules.cases.json", only: { $0.hasPrefix("drive-") }) { _, given, expect in
            if let matches = expect["matches"] {
                try Fixtures.same(matches.bool, DriveJoin.matches(matchOf(given["match"]), drive: driveOf(given["drive"])), "matches")
            } else if let entry = expect["entry"] {
                let drives = (given["drives"]?.array ?? []).map {
                    DriveEntry(id: $0["id"]?.string ?? "", match: matchOf($0["match"]), enabled: $0["enabled"]?.bool ?? true)
                }
                try Fixtures.same(entry.string, DriveJoin.entryFor(drives, drive: driveOf(given["drive"]))?.id, "entry")
            } else if let id = expect["driveId"] {
                try Fixtures.same(id.string, DriveJoin.driveId(given["identification"]?.string ?? "", device: given["device"]?.string ?? ""), "drive id")
            } else {
                return false
            }
            return true
        }
        failures += try Fixtures.runCases("domain/identity.cases.json", only: { $0.hasPrefix("drive-") }) { _, given, expect in
            try Fixtures.same(expect["shortModel"]?.string, DriveJoin.shortModel(given["identification"]?.string ?? ""), "short model")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func joinsMakemkvAndOsDrives() {
        let mk = [MakemkvDrive(index: 0, state: .inserted, flags: DiscFlags(raw: 4), identification: "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325",
                               label: "MOVIE", device: "/dev/rdisk4"),
                  MakemkvDrive(index: 1, state: .noDrive, flags: DiscFlags(raw: 0), identification: "", label: "", device: "")]
        let os = [OsDrive(device: "/dev/disk5"), OsDrive(device: "/dev/disk4", identification: "HL-DT-ST", mountPath: "/Volumes/MOVIE")]
        let joined = DriveJoin.join(mk, osDrives: os)
        #expect(joined.count == 2)
        #expect(joined.first?.driveId == "drv-86920bcfd062b640")
        #expect(joined.first?.os?.mountPath == "/Volumes/MOVIE")
        #expect(joined.last?.makemkv == nil)
        #expect(joined.last?.driveId == DriveJoin.driveId("", device: "/dev/disk5"))
    }

    @Test func accumulatorStopsOnSpaceWarningAndRenumberedDrives() {
        var acc = RunAccumulator(readsData: true, expectedIndex: 0, expectedDevice: "/dev/rdisk4")
        acc.feed(Robot.parseLine("DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk4\"")!)
        #expect(acc.stopReason == nil)
        acc.feed(Robot.parseLine("MSG:1004,0,1,\"Debug logging enabled, log will be saved as file:///Users/me/My%20Logs/MakeMKV_log.txt\",\"%1\",\"file:///Users/me/My%20Logs/MakeMKV_log.txt\"")!)
        #expect(acc.debugLog == "/Users/me/My Logs/MakeMKV_log.txt")
        acc.feed(Robot.parseLine("DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk5\"")!)
        #expect(acc.stopReason?.code == .driveRenumbered)
        acc.feed(Robot.parseLine("MSG:2003,516,3,\"Error 'Scsi error - MEDIUM ERROR' occurred while reading\",\"x\",\"a\",\"b\",\"c\"")!)
        #expect(acc.readErrors.count == 1)
        #expect(acc.firstError?.code == 2003)
        var space = RunAccumulator()
        space.feed(Robot.parseLine("MSG:5038,0,0,\"The total size of all output files may reach as much as 40 Gb while there are only 10 Gb free\",\"x\"")!)
        #expect(space.stopReason?.code == .spaceMakemkvWarning)
        var windows = RunAccumulator()
        windows.feed(.message(RobotMessage(code: 1004, flags: 0, count: 1, text: "x", format: "%1", params: ["file:///C:/Users/me/log.txt"])))
        #expect(windows.debugLog == "C:/Users/me/log.txt")
    }
}
