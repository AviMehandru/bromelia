import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/engine/makemkv-service.cases.json: MakemkvService.invocation behind a real ConfigService (a fake system
/// keystore, an empty secrets folder), on a real executor.
@Suite struct MakemkvServiceTests {
    /// The text between `start` and the next `end`.
    private static func between(_ text: String, _ start: String, _ end: String) -> String {
        guard let a = text.range(of: start), let b = text[a.upperBound...].range(of: end) else { return "" }
        return String(text[a.upperBound..<b.lowerBound])
    }

    private static func projection(_ i: MakemkvInvocation) -> JsonValue {
        var xml = JsonValue.null
        if let text = i.settings.profileXml {
            xml = .object([("name", .string(between(text, "<name lang=\"eng\">", "</name>"))),
                           ("selection", .string(between(text, "app_DefaultSelectionString=\"", "\"")))])
        }
        let o = i.options
        func number(_ n: Int?) -> JsonValue { n.map { .integer(Int64($0)) } ?? .null }
        return .object([
            ("settings", .object(i.settings.settings.map { ($0.key, .string($0.value)) })),
            ("profileXml", xml),
            ("options", .object([("profilePath", o.profilePath.map { .string($0) } ?? .null), ("minLengthSeconds", number(o.minLengthSeconds)),
                                 ("cacheMB", number(o.cacheMB)), ("directIO", o.directIO.map { .bool($0) } ?? .null),
                                 ("extraArguments", .string(o.extraArguments)), ("scan", .bool(o.scan))])),
            ("workDirectory", .string(i.settings.workDirectory)),
            ("dataDir", .string(i.settings.dataDir)),
            ("stallTimeout", i.stallTimeout.map { .integer(Int64($0.seconds)) } ?? .null),
            ("transcript", i.transcript.map { .string($0) } ?? .null),
        ])
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/makemkv-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let world = World()
            do {
                let bus = EventBus(clock: world.clock)
                let problems = await ProblemService(bus: bus, clock: world.clock)
                let secrets = Dictionary(uniqueKeysWithValues: (given["secrets"]?.members ?? []).map { ($0.key, $0.value.string!) })
                let keystore = FakeKeystore(secrets, systemStore: true)
                for (k, v) in given["keystoreFails"]?.members ?? [] { keystore.fails[k] = v }
                let environment = Dictionary(uniqueKeysWithValues: (given["environment"]?.members ?? []).map { ($0.key, $0.value.string!) })
                let configFiles = ConfigFiles(nil)
                let config = await ConfigService(executor: world.executor, bus: bus, problems: problems, fs: configFiles, keystore: keystore,
                                                 secretFiles: FakeKeystore([:], systemStore: false), kv: FakeKv([:], gate: configFiles),
                                                 path: "/data/config.json", environment: environment)
                let tree = Tree()
                let dataDir = given["dataDir"]?.string ?? "/home/u/.MakeMKV"
                var userSettings: String? = dataDir + "/settings.conf"
                if given["userSettings"]?.isNull == true {
                    userSettings = nil
                } else if let text = given["userSettings"]?.string {
                    tree.makeDirs(dataDir)
                    tree.files[userSettings!] = Array(text.utf8)
                }
                if let fails = given["userSettingsFails"] { tree.fails.append((op: "read", path: userSettings!, error: fails, times: -1)) }
                let service = await MakemkvService(fs: tree, config: config, dataDir: dataDir, userSettings: userSettings, home: "/home/u")

                let decoded = try ConfigCodec.decode(JsonValue.encodeCanonical(given["config"]!))
                let profile = ProfileResolver.resolve(decoded, driveId: given["configId"]?.string, facts: RuleFacts(name: "", label: "", formatCode: ""),
                                                      sessionChoices: nil)
                var got: JsonValue
                do throws(BroError) {
                    got = Self.projection(try service.invocation(decoded, profile: profile, selectionOverride: given["selectionOverride"]?.string,
                                                                 jobDirectory: given["jobDirectory"]?.string ?? "/data/jobs/j1"))
                } catch {
                    got = .object([("error", .object([("code", .string(error.code)), ("params", .object(error.params))]))])
                }
                let want = String(decoding: JsonValue.encodeCanonical(Fixtures.sorted(expect)), as: UTF8.self)
                let have = String(decoding: JsonValue.encodeCanonical(Fixtures.sorted(got)), as: UTF8.self)
                try Fixtures.check(want == have, "expected\n\(want)got\n\(have)")
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
