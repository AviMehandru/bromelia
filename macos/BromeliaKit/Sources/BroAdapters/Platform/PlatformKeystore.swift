import BroDomain
import BroFoundation
import BroPorts
import Foundation
import LocalAuthentication
import Security

/// Keystore on macOS (plan §10.3): generic passwords in the login Keychain (service "Bromelia", account the secret's
/// name), never with UI: a locked keychain fails instead of asking. When there is no keychain at all (a daemon started
/// before anyone logged in), the secrets are files <fallbackDir>/<name> (0600, in a 0700 folder) instead; that is
/// decided once, at the first call. A fallback folder on a network share (a file system without MNT_LOCAL), or one
/// that doesn't stay 0700 and the user's, is refused (fs.notPrivate) for reading and writing; removing still works.
/// Names are SecretRef names (keystore.cases.json); anything else fails with keystore.failed.
public final class PlatformKeystore: Keystore, @unchecked Sendable {
    private let fallbackDir: String
    private let service: String
    private let lock = NSLock()
    private var system: Bool?

    /// `service`: the Keychain service (tests use their own). `systemStore` false: always the files (tests).
    public init(fallbackDir: String, service: String = "Bromelia", systemStore: Bool = true) {
        self.fallbackDir = fallbackDir
        self.service = service
        if !systemStore { system = false }
    }

    /// Where the secrets are: "keychain" or "file".
    public func backend() -> String { useSystem() ? "keychain" : "file" }

    public func get(_ name: String) throws(BroError) -> String? {
        try check(name)
        guard useSystem() else { return try readFile(name) }
        var query = item(name)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = Self.withoutUI { SecItemCopyMatching(query as CFDictionary, &result) }
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess else { throw failed(name, status) }
        return String(decoding: (result as? Data) ?? Data(), as: UTF8.self)
    }

    public func set(_ name: String, value: String) throws(BroError) {
        try check(name)
        guard useSystem() else { return try writeFile(name, value) }
        let data = Data(value.utf8)
        var status = Self.withoutUI { SecItemUpdate(item(name) as CFDictionary, [kSecValueData as String: data] as CFDictionary) }
        if status == errSecItemNotFound {
            var add = item(name)
            add[kSecValueData as String] = data
            add[kSecAttrLabel as String] = "\(service): \(name)"
            status = Self.withoutUI { SecItemAdd(add as CFDictionary, nil) }
        }
        guard status == errSecSuccess else { throw failed(name, status) }
    }

    public func remove(_ name: String) throws(BroError) {
        try check(name)
        guard useSystem() else {
            if unlink(filePath(name)) != 0 && errno != ENOENT { throw failed(name, String(cString: strerror(errno))) }
            return
        }
        let status = Self.withoutUI { SecItemDelete(item(name) as CFDictionary) }
        guard status == errSecSuccess || status == errSecItemNotFound else { throw failed(name, status) }
    }

    // MARK: - Keychain

    private func item(_ name: String) -> [String: Any] {
        let context = LAContext()
        context.interactionNotAllowed = true
        return [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: name,
                kSecUseAuthenticationContext as String: context]
    }

    /// The Keychain, unless there is none to use (asked once).
    private func useSystem() -> Bool {
        lock.withLock {
            if let system { return system }
            var query = item("bromelia.probe")
            query[kSecMatchLimit as String] = kSecMatchLimitOne
            let status = Self.withoutUI { SecItemCopyMatching(query as CFDictionary, nil) }
            let usable = status != errSecNoSuchKeychain && status != errSecNotAvailable
            system = usable
            return usable
        }
    }

    /// The file-based login keychain ignores `interactionNotAllowed` and shows its unlock dialog when it is locked
    /// (found with a stored secret and a locked keychain); SecKeychainSetUserInteractionAllowed is the only switch for that
    /// dialog. It is deprecated without a replacement, so it is looked up with dlsym (no warning), turned off around each
    /// call and restored, under one lock (the setting is per process).
    private static let interactionLock = NSLock()
    private typealias GetInteraction = @convention(c) (UnsafeMutablePointer<DarwinBoolean>) -> OSStatus
    private typealias SetInteraction = @convention(c) (DarwinBoolean) -> OSStatus
    nonisolated(unsafe) private static let security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW)
    private static let getInteraction = dlsym(security, "SecKeychainGetUserInteractionAllowed").map { unsafeBitCast($0, to: GetInteraction.self) }
    private static let setInteraction = dlsym(security, "SecKeychainSetUserInteractionAllowed").map { unsafeBitCast($0, to: SetInteraction.self) }

    private static func withoutUI<T>(_ body: () -> T) -> T {
        interactionLock.lock()
        defer { interactionLock.unlock() }
        var was: DarwinBoolean = true
        guard let getInteraction, let setInteraction, getInteraction(&was) == errSecSuccess else { return body() }
        _ = setInteraction(false)
        defer { _ = setInteraction(was) }
        return body()
    }

    // MARK: - Files

    private func filePath(_ name: String) -> String { (fallbackDir as NSString).appendingPathComponent(name) }

    private func readFile(_ name: String) throws(BroError) -> String? {
        try refuseNetwork()
        guard let data = FileManager.default.contents(atPath: filePath(name)) else {
            if access(filePath(name), F_OK) != 0 && errno == ENOENT { return nil }
            throw failed(name, String(cString: strerror(errno)))
        }
        return String(decoding: data, as: UTF8.self)
    }

    /// Written next to the final file (0600 from the start), then renamed over it.
    private func writeFile(_ name: String, _ value: String) throws(BroError) {
        try refuseNetwork()
        if mkdir(fallbackDir, 0o700) != 0 && errno != EEXIST { throw failed(name, String(cString: strerror(errno))) }
        var st = Darwin.stat()
        guard chmod(fallbackDir, 0o700) == 0, lstat(fallbackDir, &st) == 0, (st.st_mode & S_IFMT) == S_IFDIR,
              st.st_uid == geteuid(), st.st_mode & 0o077 == 0 else {
            throw notPrivate("the file system didn't keep its owner-only permissions")
        }
        let temp = (fallbackDir as NSString).appendingPathComponent(".\(name).\(UUID().uuidString).tmp")
        let fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw failed(name, String(cString: strerror(errno))) }
        let bytes = Array(value.utf8)
        var written = 0
        while written < bytes.count {
            let n = bytes.withUnsafeBytes { write(fd, $0.baseAddress! + written, bytes.count - written) }
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break }
            written += n
        }
        let ok = written == bytes.count && fsync(fd) == 0
        let reason = String(cString: strerror(errno))
        close(fd)
        guard ok, rename(temp, filePath(name)) == 0 else {
            let why = ok ? String(cString: strerror(errno)) : reason
            unlink(temp)
            throw failed(name, why)
        }
    }

    /// fs.notPrivate when the fallback folder (or the nearest folder above it that exists) is on a network share: the
    /// server decides who reads the files there, whatever mode is asked for.
    private func refuseNetwork() throws(BroError) {
        var path = fallbackDir
        var fs = statfs()
        while statfs(path, &fs) != 0 {
            let up = (path as NSString).deletingLastPathComponent
            if up == path || up.isEmpty { return }
            path = up
        }
        if fs.f_flags & UInt32(MNT_LOCAL) == 0 { throw notPrivate("it is on a network share") }
    }

    // MARK: - Names and errors

    /// A SecretRef name: ^[a-z][a-zA-Z0-9]*(\.[A-Za-z0-9_-]+)*$.
    static func isSecretName(_ name: String) -> Bool {
        let s = Array(name.utf8)
        func alnum(_ c: UInt8) -> Bool { (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) }
        guard let first = s.first, first >= 0x61, first <= 0x7A else { return false }
        var i = 1
        while i < s.count, alnum(s[i]) { i += 1 }
        while i < s.count {
            guard s[i] == UInt8(ascii: ".") else { return false }
            i += 1
            let start = i
            while i < s.count, alnum(s[i]) || s[i] == UInt8(ascii: "_") || s[i] == UInt8(ascii: "-") { i += 1 }
            if i == start { return false }
        }
        return true
    }

    private func check(_ name: String) throws(BroError) {
        guard Self.isSecretName(name) else { throw failed(name, "not a secret name") }
    }

    private func failed(_ name: String, _ status: OSStatus) -> BroError {
        failed(name, (SecCopyErrorMessageString(status, nil) as String?) ?? "Keychain error \(status)")
    }

    private func failed(_ name: String, _ reason: String) -> BroError {
        BroMessage(.keystoreFailed, [("name", .string(name)), ("reason", .string(reason))], severity: .error).toError()
    }

    private func notPrivate(_ reason: String) -> BroError {
        BroMessage(.fsNotPrivate, [("path", .string(fallbackDir)), ("reason", .string(reason))], severity: .error).toError()
    }
}
