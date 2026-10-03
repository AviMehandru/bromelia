import BroDomain
import BroFoundation
import BroTestSupport
import Testing

private func strings(_ v: JsonValue?) -> [String]? { v?.array?.map { $0.string ?? "" } }

private func facts(_ f: JsonValue) -> RuleFacts {
    RuleFacts(name: f["name"]?.string ?? "", label: f["label"]?.string ?? "", formatCode: f["formatCode"]?.string ?? "", kind: f["kind"]?.string,
              driveId: f["driveId"]?.string, profileId: f["profileId"]?.string, automatic: f["automatic"]?.bool ?? false)
}

/// Every key of `expected` is in `actual` with the same value (objects compared the same way, anything else exactly).
private func subset(_ expected: JsonValue, _ actual: JsonValue?, _ path: String) throws {
    if let members = expected.members {
        for m in members { try subset(m.value, actual?[m.key], path + "." + m.key) }
        return
    }
    try Fixtures.same(expected, actual, path)
}

struct RulesTests {
    @Test func ruleCases() throws {
        let failures = try Fixtures.runCases("domain/rules.cases.json", only: { !$0.hasPrefix("drive-") && $0 != "invalid-regex-issue" }) { id, given, expect in
            if let w = given["when"] {
                let when = RuleWhen(nameOrLabel: w["nameOrLabel"]?.string, name: w["name"]?.string, label: w["label"]?.string, formats: strings(w["formats"]),
                                    kinds: strings(w["kinds"]), drives: strings(w["drives"]), profiles: strings(w["profiles"]), automatic: w["automatic"]?.bool)
                try Fixtures.same(expect["matches"]?.bool, RuleMatcher.matches(when, facts: facts(given["facts"]!)), "matches")
                return true
            }
            if id.hasPrefix("mode-") {
                var raw = 0
                for f in strings(given["flags"]) ?? [] {
                    switch f {
                    case "dvdFiles": raw |= 1
                    case "hdDvdFiles": raw |= 2
                    case "blurayFiles": raw |= 4
                    default: try Fixtures.check(false, "flag \(f)")
                    }
                }
                let mode = ModeChooser.mode(given["format"]?.string.flatMap(DiscFormat.init(rawValue:)), flags: DiscFlags(raw: raw),
                                            content: given["content"]?.string.flatMap(DiscContent.init(rawValue:)) ?? .unknown, profile: Profile(given["profile"]!),
                                            chosenByHand: false)
                try Fixtures.same(expect["mode"], mode.map { .string($0.rawValue) } ?? .null, "mode")
                return true
            }
            if let config = given["config"] {
                let effective = ProfileResolver.resolve(try decodeConfig(config), driveId: given["drive"]?.string, facts: facts(given["facts"]!),
                                                        sessionChoices: given["session"])
                try subset(expect["profile"]!, effective.profile, "profile")
                try Fixtures.same(expect["steps"], .array(effective.steps.map { .string($0) }), "steps")
                try Fixtures.same(expect["trace"], .array(effective.trace.map { t in
                    .object([("layer", .string(t.layer.rawValue))] + (t.id.map { [("id", JsonValue.string($0))] } ?? []))
                }), "trace")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func stepFilterCases() throws {
        let failures = try Fixtures.runCases("domain/arguments.cases.json", only: { $0.hasPrefix("run-condition") || $0 == "status-words" }) { _, given, expect in
            if let outcomes = given["outcomes"]?.array {
                try Fixtures.same(expect["statusWords"], .array(outcomes.map { .string(StepFilter.statusWord(Outcome(rawValue: $0.string ?? "")!).rawValue) }),
                                  "statusWords")
                return true
            }
            let step = StepDefinition.decode(.object([("id", .string("s")), ("kind", .string("command"))] + (given["step"]?.members ?? [])))
            try Fixtures.same(expect["runs"]?.bool, StepFilter.applies(step, outcome: Outcome(rawValue: given["outcome"]?.string ?? "")!), "runs")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func ruleProfileSwitchAndRemovedKeys() throws {
        let config = try decodeConfig(JsonValue.parse("""
            {"version": 3,
             "profiles": [{"id": "default", "titles": {"strategy": "longest"}}, {"id": "anime", "titles": {"strategy": "all"}}],
             "rules": [{"id": "a", "when": {"nameOrLabel": "piece"}, "then": {"profile": "anime"}},
                       {"id": "b", "when": {"profiles": ["anime"]}, "then": {"set": {"titles": {"strategy": null}}}},
                       {"id": "off", "enabled": false, "when": {}, "then": {"steps": ["x"]}}]}
            """)!)
        let effective = ProfileResolver.resolve(config, driveId: nil, facts: RuleFacts(name: "One Piece", label: "", formatCode: "DVD"), sessionChoices: nil)
        #expect(effective.trace.map(\.layer) == [.defaults, .defaultProfile, .driveProfile, .rule, .rule])
        // Rule b removed the strategy, so the default comes back rather than the default profile's.
        let defaults = ProfileResolver.resolve(try decodeConfig(JsonValue.parse(#"{"version": 3}"#)!), driveId: nil,
                                               facts: RuleFacts(name: "", label: "", formatCode: ""), sessionChoices: nil).profile
        #expect(defaults["titles"]?["strategy"] == effective.profile["titles"]?["strategy"])
        #expect(effective.trace[4].keys == ["titles.strategy"])
        #expect(effective.steps.isEmpty)
    }
}
