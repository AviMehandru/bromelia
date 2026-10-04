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
}
