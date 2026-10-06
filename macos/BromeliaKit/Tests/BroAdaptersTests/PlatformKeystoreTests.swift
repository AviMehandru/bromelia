import BroAdapters
import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/keystore.cases.json against the login Keychain and against the fallback files.
struct PlatformKeystoreTests {
    func text(_ v: JsonValue?) -> String? {
        guard let v, !v.isNull else { return nil }
        if let s = v.string { return s }
        return String(repeating: v["repeat"]!.string!, count: Int(v["times"]!.int!))
    }

    func scratch() -> String { (Fixtures.temporaryDirectory as NSString).appendingPathComponent("BromeliaTest-\(UUID().uuidString)") }

    /// The temporary folder is on a network share (BROMELIA_TEST_TMPDIR on an SMB mount): the fallback files are
    /// refused there.
    var onShare: Bool {
        var fs = statfs()
        return statfs(Fixtures.temporaryDirectory, &fs) == 0 && fs.f_flags & UInt32(MNT_LOCAL) == 0
    }

    func runAll(systemStore: Bool) throws {
        let doc = try Fixtures.json("adapters/keystore.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let service = "BromeliaTest-\(UUID().uuidString)", dir = scratch()
            let keystore = PlatformKeystore(fallbackDir: dir, service: service, systemStore: systemStore)
            let steps = c["given"]!["steps"]!.array!, results = c["expect"]!["results"]!.array!
            // Fallback files on a network share: every read and write is refused, removing still works.
            let refused = !systemStore && onShare
            do {
                for (i, step) in steps.enumerated() {
                    let name = step["name"]!.string!, want = results[i], op = step["op"]!.string!
                    if refused && want["error"] == nil && op != "remove" {
                        do {
                            if op == "get" { _ = try keystore.get(name) } else { try keystore.set(name, value: text(step["value"])!) }
                            try Fixtures.check(false, "step \(i): expected fs.notPrivate")
                        } catch let error as BroError {
                            try Fixtures.same("fs.notPrivate", error.code, "step \(i)")
                        }
                        try Fixtures.check(((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).isEmpty, "nothing written")
                        continue
                    }
                    do {
                        var got: String?
                        switch step["op"]!.string! {
                        case "get": got = try keystore.get(name)
                        case "set": try keystore.set(name, value: text(step["value"])!)
                        default: try keystore.remove(name)
                        }
                        try Fixtures.check(want["error"] == nil, "step \(i): expected \(want)")
                        try Fixtures.same(text(want), got, "step \(i)")
                    } catch let error as BroError {
                        try Fixtures.same(want["error"]?.string, error.code, "step \(i)")
                        try Fixtures.same(.string(name), JsonValue.object(error.params)["name"] ?? .null, "name")
                    }
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
            for step in steps { try? keystore.remove(step["name"]!.string!) }
            try? FileManager.default.removeItem(atPath: dir)
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// `security find-generic-password` (attributes only, never the secret): whether the item exists.
    func inKeychain(service: String, account: String) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/security")
        p.arguments = ["find-generic-password", "-s", service, "-a", account]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        try? p.run()
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    @Test func theSharedCasesPassInTheKeychain() throws {
        #expect(PlatformKeystore(fallbackDir: scratch()).backend() == "keychain")
        try runAll(systemStore: true)
    }

    @Test func theSharedCasesPassInTheFallbackFiles() throws {
        try runAll(systemStore: false)
    }

    @Test func secretsGoToTheKeychain() throws {
        let service = "BromeliaTest-\(UUID().uuidString)", dir = scratch()
        let keystore = PlatformKeystore(fallbackDir: dir, service: service)
        try keystore.set("metadata.tmdb", value: "abc123")
        #expect(inKeychain(service: service, account: "metadata.tmdb"))
        #expect(!FileManager.default.fileExists(atPath: dir))
        #expect(try PlatformKeystore(fallbackDir: dir, service: service).get("metadata.tmdb") == "abc123")
        try keystore.remove("metadata.tmdb")
        #expect(!inKeychain(service: service, account: "metadata.tmdb"))
    }

    @Test func theFallbackIsOneFilePerSecretOnlyTheOwnerCanRead() throws {
        let dir = scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let keystore = PlatformKeystore(fallbackDir: dir, systemStore: false)
        #expect(keystore.backend() == "file")
        if onShare {
            // On a network share the server decides who reads the files: nothing is written.
            #expect(throws: BroError.self) { try keystore.set("metadata.tmdb", value: "abc123") }
            #expect(((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).isEmpty)
            return
        }
        try keystore.set("metadata.tmdb", value: "abc123")
        try keystore.set("metadata.tmdb", value: "def456")
        #expect(try FileManager.default.contentsOfDirectory(atPath: dir) == ["metadata.tmdb"])
        #expect(try String(contentsOfFile: dir + "/metadata.tmdb", encoding: .utf8) == "def456")
        let file = try FileManager.default.attributesOfItem(atPath: dir + "/metadata.tmdb")
        let folder = try FileManager.default.attributesOfItem(atPath: dir)
        #expect((file[.posixPermissions] as? Int) == 0o600)
        #expect((folder[.posixPermissions] as? Int) == 0o700)
    }

    /// A fallback folder that already exists with looser permissions is made 0700 again before a secret goes in.
    @Test func aLooseFallbackFolderIsMadeOwnerOnly() throws {
        if onShare { return }
        let dir = scratch()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        #expect(mkdir(dir, 0o755) == 0 && chmod(dir, 0o755) == 0)
        try PlatformKeystore(fallbackDir: dir, systemStore: false).set("metadata.tmdb", value: "abc123")
        #expect((try FileManager.default.attributesOfItem(atPath: dir)[.posixPermissions] as? Int) == 0o700)
    }
}
