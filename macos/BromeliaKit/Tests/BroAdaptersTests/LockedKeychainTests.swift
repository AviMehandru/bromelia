import BroAdapters
import BroDomain
import BroFoundation
import Foundation
import Testing

/// With the login keychain locked (the owner allows it; BROMELIA_TEST_LOCKED_KEYCHAIN), every call fails at once with
/// keystore.failed instead of showing a password dialog, and nothing falls back to the files. Unlocking needs the
/// owner's password, so the test leaves the keychain locked.
struct LockedKeychainTests {
    static let enabled = !(ProcessInfo.processInfo.environment["BROMELIA_TEST_LOCKED_KEYCHAIN"] ?? "").isEmpty

    @Test(.enabled(if: enabled)) func aLockedKeychainFailsWithoutADialog() throws {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("BromeliaTest-\(UUID().uuidString)")
        let keystore = PlatformKeystore(fallbackDir: dir, service: "BromeliaTest-\(UUID().uuidString)")
        #expect(keystore.backend() == "keychain")
        let started = Date()
        var codes: [String] = []
        do throws(BroError) { try keystore.set("metadata.tmdb", value: "abc123") } catch { codes.append(error.code) ; print("set: \(error.code) \(error.params)") }
        do throws(BroError) { _ = try keystore.get("metadata.tmdb") } catch { codes.append(error.code); print("get: \(error.code) \(error.params)") }
        do throws(BroError) { try keystore.remove("metadata.tmdb") } catch { codes.append(error.code); print("remove: \(error.code) \(error.params)") }
        let seconds = Date().timeIntervalSince(started)
        print("codes \(codes) in \(seconds) s")
        #expect(codes.contains("keystore.failed"))
        #expect(seconds < 10)
        #expect(!FileManager.default.fileExists(atPath: dir))
    }

    /// A secret stored while the keychain is unlocked, then read and removed after locking it (BROMELIA_TEST_LOCK_KEYCHAIN:
    /// the test locks the login keychain itself). Both fail at once with keystore.failed; nothing comes from the files.
    /// The test secret (service BromeliaLockedTest) stays in the keychain until the next unlocked run removes it.
    @Test(.enabled(if: !(ProcessInfo.processInfo.environment["BROMELIA_TEST_LOCK_KEYCHAIN"] ?? "").isEmpty))
    func aStoredSecretCantBeReadWhileLocked() throws {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("BromeliaTest-\(UUID().uuidString)")
        let keystore = PlatformKeystore(fallbackDir: dir, service: "BromeliaLockedTest")
        try? keystore.remove("metadata.tmdb")
        try keystore.set("metadata.tmdb", value: "abc123")
        #expect(try keystore.get("metadata.tmdb") == "abc123")
        let lock = Process()
        lock.executableURL = URL(fileURLWithPath: "/usr/bin/security")
        lock.arguments = ["lock-keychain", NSHomeDirectory() + "/Library/Keychains/login.keychain-db"]
        try lock.run()
        lock.waitUntilExit()
        #expect(lock.terminationStatus == 0)
        let started = Date()
        var got: String?, getError: String?, removeError: String?
        do throws(BroError) { got = try keystore.get("metadata.tmdb") } catch { getError = error.code; print("get: \(error.code) \(error.params)") }
        do throws(BroError) { try keystore.remove("metadata.tmdb") } catch { removeError = error.code; print("remove: \(error.code) \(error.params)") }
        let seconds = Date().timeIntervalSince(started)
        print("locked: get \(getError ?? "value \(got ?? "none")"), remove \(removeError ?? "ok") in \(seconds) s")
        #expect(getError == "keystore.failed")
        #expect(got == nil)
        #expect(seconds < 10)
        #expect(!FileManager.default.fileExists(atPath: dir))
    }
}
