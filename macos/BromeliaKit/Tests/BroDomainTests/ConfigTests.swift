import BroDomain
import BroFoundation
import BroTestSupport
import Testing

func decodeConfig(_ json: JsonValue) throws -> Config { try ConfigCodec.decode(JsonValue.encodeCanonical(json)) }

func encodedConfig(_ c: Config) -> String { String(decoding: ConfigCodec.encode(c), as: UTF8.self) }

/// Issues as the fixtures write them: params only when the expected issue has them.
func sameIssues(_ expected: JsonValue?, _ actual: [BroDomain.Issue]) throws {
    let want = expected?.array ?? []
    let got: [JsonValue] = actual.enumerated().map { i, x in
        let j = x.toJson()
        if i < want.count && want[i]["params"] != nil { return j }
        return .object((j.members ?? []).filter { $0.key != "params" })
    }
    try Fixtures.same(expected, .array(got), "issues")
}

struct ConfigTests {
    @Test func configCodecCases() throws {
        let failures = try Fixtures.runCases("config/config-codec.cases.json") { _, given, expect in
            if given["template"] != nil {
                try Fixtures.same(Fixtures.sorted(expect["profile"]!), Fixtures.sorted(ConfigTemplates.archiveEverything().json), "profile")
                return true
            }
            if let catalog = given["catalog"]?.string {
                // The settings catalog the forms are generated from (no Domain function reads it: it's the apps').
                let doc = try Fixtures.sharedJson(String(catalog.dropFirst("shared/".count)))
                try Fixtures.check(doc.description.contains("\"\(expect["hasKey"]?.string ?? "")\""), "catalog key")
                try Fixtures.check(!(doc["selectionPresets"]?.array ?? []).isEmpty, "selection presets")
                return true
            }
            let config = try given["file"]?.string.map { try ConfigCodec.decode(try Fixtures.bytes("config/" + $0)) } ?? decodeConfig(given["json"]!)
            let encoded = encodedConfig(config)
            if let eq = expect["equals"]?.string { try Fixtures.same(try Fixtures.text("config/" + eq), encoded, "encoded") }
            if expect["roundTrips"]?.bool == true {
                // Decoding what was encoded changes nothing (the file itself is formatted by hand).
                try Fixtures.same(encoded, encodedConfig(try ConfigCodec.decode(ConfigCodec.encode(config))), "round trip")
            }
            for (path, want) in expect["paths"]?.members ?? [] { try Fixtures.same(want, Fixtures.at(config.document, path), path) }
            for c in expect["contains"]?.array ?? [] { try Fixtures.check(encoded.contains(c.string ?? ""), "encoded doesn't contain \(c)") }
            for c in expect["encodedWithout"]?.array ?? [] { try Fixtures.check(!encoded.contains("\"\(c.string ?? "")\""), "encoded contains \(c)") }
            if expect["profileStaysSparse"]?.bool == true {
                try Fixtures.same(Fixtures.sorted(given["json"]!["profiles"]!.array![0]), Fixtures.sorted(config.document["profiles"]!.array![0]), "sparse profile")
            }
            if let issues = expect["issues"] { try sameIssues(issues, ConfigValidator.validate(config)) }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func ruleRegexIssues() throws {
        let failures = try Fixtures.runCases("domain/rules.cases.json", only: { $0 == "invalid-regex-issue" }) { _, given, expect in
            // A fragment of a version 3 document.
            let full = JsonValue.object([("version", .integer(3))] + (given["config"]?.members ?? []))
            try sameIssues(expect["issues"], ConfigValidator.validate(try decodeConfig(full)))
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func typedViewsFillDefaults() throws {
        let config = try decodeConfig(JsonValue.parse("""
            {"version": 3,
             "profiles": [{"id": "default", "titles": {"strategy": "longest"}, "naming": {"layout": "mediaServer"}}],
             "drives": [{"id": "left", "match": {"devicePath": "/dev/sr0"}, "profile": "default"}],
             "steps": [{"id": "enc", "kind": "handbrake"}, {"id": "cmd", "kind": "command", "command": {"executable": "/bin/echo"}}],
             "rules": [{"id": "bd", "when": {"formats": ["BR*"]}, "then": {"steps": ["enc"]}}]}
            """)!)
        #expect(ConfigValidator.validate(config).isEmpty)
        let steps = Config.steps(config)
        #expect(steps[0].background)
        #expect(steps[0].handbrake.preset == "H.265 MKV 1080p30")
        #expect(!steps[1].background)
        #expect(steps[1].command.arguments == "{outputDir}")
        #expect(Config.drives(config)[0].id == "left")
        #expect(Config.rules(config)[0].when.formats == ["BR*"])
        let profile = Config.profiles(config)[0]
        #expect(TitleSettings.decode(profile.json["titles"]!).strategy == .longest)
        #expect(NamingSettings.decode(profile.json["naming"]!).layout == .mediaServer)
        let bad = try decodeConfig(JsonValue.parse("""
            {"version": 3, "drives": [{"id": "d", "profile": "nope"}], "steps": [{"id": "s", "kind": "command"}, {"id": "s", "kind": "command"}],
             "server": {"tls": {"certificate": "/c.pem"}}}
            """)!)
        #expect(ConfigValidator.validate(bad).map(\.code.rawValue) == ["config.duplicateId", "config.unknownReference", "config.tlsNeedsBoth"])
    }
}
