import BroDomain
import BroFoundation
import BroTestSupport
import Testing

private func valuesOf(_ v: JsonValue?) -> [String: String] {
    Dictionary(uniqueKeysWithValues: (v?.members ?? []).map { ($0.key, $0.value.string ?? "") })
}

struct NamingTests {
    @Test func namingCases() throws {
        let failures = try Fixtures.runCases("domain/naming.cases.json") { _, given, expect in
            if let template = given["template"]?.string {
                let values = valuesOf(given["values"])
                if let r = expect["render"] {
                    try Fixtures.same(r.string, TemplateEngine.render(template, values: values), "render")
                } else {
                    try Fixtures.same(expect["renderPath"]?.string, TemplateEngine.renderPath(template, values: values), "renderPath")
                }
                return true
            }
            if let component = given["component"]?.string {
                try Fixtures.same(expect["sanitize"]?.string, Sanitizer.component(component), "sanitize")
                return true
            }
            if let layout = given["layout"]?.string {
                let naming = NamingSettings(layout: Layout(rawValue: layout)!)
                var values = valuesOf(given["values"])
                values["kind"] = given["kind"]?.string ?? ""
                let folder = Layouts.folder(naming, values: values)
                let outputs: [(String, PlannedOutput)] = [
                    ("episodePath", PlannedOutput(role: .episode)), ("mainFeature", PlannedOutput(role: .title, mainFeature: true)),
                    ("extra", PlannedOutput(role: .title)), ("backup", PlannedOutput(role: .backup)),
                ]
                let paths = Layouts.paths(naming, values: values, outputs: outputs.map(\.1))
                if let f = expect["folder"] { try Fixtures.same(f.string, folder, "folder") }
                for (i, (key, _)) in outputs.enumerated() {
                    if let want = expect[key]?.string { try Fixtures.same(folder + "/" + want, paths[i].path, key) }
                }
                return true
            }
            if let title = given["title"] {
                try Fixtures.same(expect["trackLabel"]?.string, TokenRegistry.trackLabel(titleOf(title)), "track label")
                return true
            }
            if let episode = given["episode"]?.int {
                try Fixtures.same(expect["episodeLabel"]?.string, TokenRegistry.episodeLabel(Int(episode), width: Int(given["width"]?.int ?? 0)), "episode label")
                return true
            }
            if let name = given["name"]?.string {
                let existing = (given["existing"]?.array ?? []).compactMap(\.string)
                try Fixtures.same(expect["next"]?.string, ConflictNamer.next(name, existing: existing, isFolder: !name.contains(".")), "next")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func argumentCases() throws {
        let failures = try Fixtures.runCases("domain/arguments.cases.json", only: { $0.hasPrefix("split") || $0.hasPrefix("quote") }) { _, given, expect in
            if let text = given["text"]?.string {
                try Fixtures.same(expect["split"], .array(ArgumentSplitter.split(text).map(JsonValue.string)), "split")
            } else {
                try Fixtures.same(expect["quoted"]?.string, ArgumentSplitter.quote(given["quote"]?.string ?? ""), "quoted")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func tokensAndTemplateLayout() {
        let identity = Identity.resolve(IdentityInputs(listing: nil, discLabel: "ONE_PIECE_S2_P7_D2", encrypted: false, format: .dvd))
        let values = TokenRegistry.values(identity, context: TokenContext(rip: "Rip", drive: "Left", disc: "ONE_PIECE_S2_P7_D2", volume: "ONE_PIECE_S2_P7_D2",
                                                                          type: .dvd, job: "3f2504e0",
                                                                          localTime: LocalTime(year: 2026, month: 10, day: 3, hour: 9, minute: 5, second: 7)))
        #expect(values["name"] == "One Piece")
        #expect(values["discLabel"] == "Season 2 Part 7 Disc 2")
        #expect(values["date"] == "2026-10-03")
        #expect(values["time"] == "09-05-07")
        #expect(values["libraryFolder"] == "TV Shows")
        let paths = Layouts.paths(NamingSettings(), values: values, outputs: [
            PlannedOutput(role: .episode, values: ["episode": "Episode 138", "track": "Title 11 Ch 1-7"], title: 0, episode: 138, extension: ".mkv"),
            PlannedOutput(role: .backup),
        ])
        #expect(paths[0].path == "One Piece - Season 2 Part 7 Disc 2/One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD.mkv")
        #expect(paths[0].episode == 138)
        #expect(paths[1].path == "One Piece - Season 2 Part 7 Disc 2/backup")
    }
}
