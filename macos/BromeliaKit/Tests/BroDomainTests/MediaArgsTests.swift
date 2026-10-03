import BroDomain
import BroFoundation
import BroTestSupport
import Testing

private func strings(_ v: JsonValue?) -> [String] { (v?.array ?? []).map { $0.string ?? "" } }

private func stringsJson(_ s: [String]) -> JsonValue { .array(s.map { .string($0) }) }

private func values(_ v: JsonValue?) -> [String: String] {
    Dictionary((v?.members ?? []).map { ($0.key, $0.value.string ?? "") }, uniquingKeysWith: { $1 })
}

private func near(_ expected: JsonValue?, _ actual: Double, _ what: String) throws {
    try Fixtures.check(abs((expected?.double ?? .nan) - actual) <= 1e-9, "\(what): expected \(String(describing: expected)), got \(actual)")
}

private func titleWithTracks(_ t: JsonValue, _ kinds: [TrackKind]) -> Title {
    var title = titleOf(t)
    title.tracks = kinds.enumerated().map { Track(index: $0.offset, kind: $0.element, codec: "", language: "", languageName: "", name: "", isDefault: false, attributes: [:]) }
    return title
}

private func step(_ given: JsonValue, kind: String = "command") -> StepDefinition {
    StepDefinition.decode(.object([("id", .string("s")), ("kind", .string(kind))] + (given.members ?? []).filter { $0.key != "kind" }))
}

private func trackKind(_ s: String) -> TrackKind { s.lowercased().hasPrefix("subtitle") ? .subtitle : TrackKind(rawValue: s.lowercased())! }

struct MediaArgsTests {
    @Test func mediaArgsCases() throws {
        let failures = try Fixtures.runCases("domain/media-args.cases.json") { id, given, expect in
            if id.hasPrefix("probe-") {
                let p = MkvProbe.parse(given["json"]?.string ?? "")
                guard let want = expect["probe"], !want.isNull else {
                    try Fixtures.check(p == nil, "probe should be nil")
                    return true
                }
                try near(want["durationSeconds"], p?.durationSeconds ?? .nan, "durationSeconds")
                try Fixtures.same(want["trackTypes"], stringsJson(p?.tracks.map(\.type) ?? []), "trackTypes")
                try Fixtures.same(want["chapterCount"]?.int, Int64(p?.chapterCount ?? -1), "chapterCount")
                return true
            }
            if let secs = given["expectedSeconds"]?.array {
                for (i, w) in (expect["tolerance"]?.array ?? []).enumerated() { try near(w, RipCheck.tolerance(secs[i].double ?? 0).seconds, "tolerance") }
                return true
            }
            if id.hasPrefix("check-") {
                let t = given["title"]!, p = given["probe"]!
                let probe = MkvProbe(durationSeconds: p["durationSeconds"]?.double,
                                     tracks: strings(p["trackTypes"]).enumerated().map { MkvTrack(id: $0.offset, type: $0.element) },
                                     chapterCount: Int(p["chapterCount"]?.int ?? 0))
                let r = RipCheck.check(probe, title: titleWithTracks(.object((t.members ?? []).filter { $0.key != "tracks" }), strings(t["tracks"]).map(trackKind)))
                if let w = expect["problems"] { try Fixtures.same(w, .array(r.problems.map { $0.toJson() }), "problems") }
                if let w = expect["notes"] { try Fixtures.same(w, .array(r.notes.map { $0.toJson() }), "notes") }
                if let w = expect["problemCodes"] { try Fixtures.same(w, stringsJson(r.problems.map { MessageCode.wire($0.code) }), "problemCodes") }
                if let w = expect["noteCodes"] { try Fixtures.same(w, stringsJson(r.notes.map { MessageCode.wire($0.code) }), "noteCodes") }
                return true
            }
            if id.hasPrefix("remux-") {
                let layout = (given["layout"]?.array ?? []).map { MkvTrack(id: Int($0["id"]?.int ?? 0), type: $0["type"]?.string ?? "") }
                let title: Title
                if let listing = given["listing"]?.string {
                    title = listingOf(try Fixtures.text(listing)).titles.first { $0.index == Int(given["title"]?.int ?? -1) }!
                } else {
                    title = titleWithTracks(.object([]), strings(given["tracks"]).map(trackKind))
                }
                let args = Remux.arguments(layout, title: title, keep: Set((given["keep"]?.array ?? []).map { Int($0.int ?? 0) }),
                                           input: given["input"]?.string ?? "", output: given["output"]?.string ?? "")
                try Fixtures.same(expect["arguments"], args.map(stringsJson) ?? .null, "arguments")
                return true
            }
            if id == "split-arguments" {
                let args = Split.arguments((given["chapters"]?.array ?? []).map { Int($0.int ?? 0) }, input: given["input"]?.string ?? "",
                                           output: given["output"]?.string ?? "")
                try Fixtures.same(expect["arguments"], stringsJson(args), "arguments")
                return true
            }
            if id == "simple-chapters" {
                let starts = SimpleChapters.parse(given["text"]?.string ?? "")
                let want = expect["starts"]?.array ?? []
                try Fixtures.check(want.count == starts.count, "count")
                for (i, w) in want.enumerated() { try near(w, starts[i].seconds, "start") }
                return true
            }
            if id.hasPrefix("handbrake-") {
                if let files = given["files"] {
                    try Fixtures.same(expect["sources"], stringsJson(strings(files).filter(HandBrakeArgs.isSource)), "sources")
                    return true
                }
                if let lines = given["lines"] {
                    var filter = ProgressFilter()
                    try Fixtures.same(expect["kept"], stringsJson(strings(lines).filter { HandBrakeArgs.keepLine(&filter, line: $0) }), "kept")
                    return true
                }
                let s = step(given["step"]!, kind: "handbrake")
                if let preset = expect["preset"] {
                    try Fixtures.same(expect["background"]?.bool, s.background, "background")
                    try Fixtures.same(preset.string, s.handbrake.preset, "preset")
                    for m in expect["runsOn"]?.members ?? [] {
                        try Fixtures.same(m.value.bool, StepFilter.applies(s, outcome: Outcome(rawValue: m.key)!), m.key)
                    }
                }
                if let output = expect["output"] {
                    try Fixtures.same(output.string, HandBrakeArgs.output(s, values: values(given["values"]), home: nil), "output")
                }
                if let arguments = expect["arguments"] {
                    try Fixtures.same(arguments, stringsJson(HandBrakeArgs.build(s, input: given["input"]?.string ?? "", output: given["output"]?.string ?? "",
                                                                                 home: nil)), "arguments")
                }
                return true
            }
            if id.hasPrefix("backup-structure") {
                var header: [UInt8]?
                if let size = given["bytes"]?.int {
                    // The file's bytes from 32769: the descriptor, else zeros.
                    let descriptor = Array((given["descriptorAt32769"]?.string ?? "").utf8)
                    header = (0..<max(0, min(5, Int(size) - 32769))).map { $0 < descriptor.count ? descriptor[$0] : 0 }
                }
                let problem = BackupStructure.problem("disc", iso: given["iso"]?.bool == true, entries: given["folder"].map(strings), isoHeader: header)
                let want = expect["problem"]
                try Fixtures.same(want?.isNull == false ? want?["code"]?.string : nil, problem.map { MessageCode.wire($0.code) }, "problem")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func commandCases() throws {
        let failures = try Fixtures.runCases("domain/arguments.cases.json", only: { $0.hasPrefix("invocation") }) { _, given, expect in
            let line = CommandArgs.build(step(given["step"]!), values: values(given["values"]), files: strings(given["files"]), home: given["home"]?.string)
            try Fixtures.same(expect["executable"]?.string, line.executable, "executable")
            try Fixtures.same(expect["arguments"], stringsJson(line.arguments), "arguments")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func otherToolArguments() throws {
        #expect(AppriseArgs.build("tgram://bot/chat", title: "T", body: "B") == ["-t", "T", "-b", "B", "tgram://bot/chat"])
        let none = JsonValue.object([])
        #expect(CdRipperArgs.build(none, device: "/dev/sr0", available: ["abcde", "cyanrip"])?.arguments == ["-d", "/dev/sr0", "-o", "flac"])
        #expect(CdRipperArgs.build(none, device: "/dev/sr0", available: ["abcde"])?.executable == "abcde")
        #expect(CdRipperArgs.build(none, device: "/dev/sr0", available: []) == nil)
        let custom = CdRipperArgs.build(.object([("audioCommand", .string("whipper cd -d {device} rip"))]), device: "/dev/sr1", available: [])
        #expect(custom?.executable == "whipper")
        #expect(custom?.arguments == ["cd", "-d", "/dev/sr1", "rip"])
        // A leading ~ is the home folder in a preset file too.
        let s = StepDefinition.decode(JsonValue.parse(#"{"id": "h", "kind": "handbrake", "handbrake": {"presetFile": "~/p.json", "preset": ""}}"#)!)
        #expect(HandBrakeArgs.build(s, input: "/i", output: "/o", home: "/home/me/") == ["--preset-import-file", "/home/me/p.json", "-i", "/i", "-o", "/o"])
    }
}
