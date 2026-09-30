import Foundation
import Network
import Security

/// A parsed HTTP request (enough of HTTP/1.1 for the web page and its JSON API).
struct HTTPRequest: Equatable {
    var method: String
    var path: String
    var query: [String: String]
    /// Lower-cased names.
    var headers: [String: String]

    /// Parses the request head. Returns nil until the whole head (up to the blank line) has arrived, and for
    /// malformed requests.
    static func parse(_ data: Data) -> HTTPRequest? {
        guard let end = data.range(of: Data("\r\n\r\n".utf8)) else { return nil }
        let head = String(decoding: data[data.startIndex..<end.lowerBound], as: UTF8.self)
        var lines = head.components(separatedBy: "\r\n")
        let first = lines.removeFirst().split(separator: " ", omittingEmptySubsequences: true).map(String.init)
        guard first.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for l in lines {
            guard let colon = l.firstIndex(of: ":") else { continue }
            headers[l[..<colon].trimmingCharacters(in: .whitespaces).lowercased()] = l[l.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let target = URLComponents(string: first[1])
        var query: [String: String] = [:]
        for q in target?.queryItems ?? [] { query[q.name] = q.value ?? "" }
        return HTTPRequest(method: first[0].uppercased(), path: target?.percentEncodedPath ?? first[1], query: query, headers: headers)
    }
}

/// Who may use the web page: with a token, whoever sends it; without one, only pages on this computer
/// (the Host header must be localhost, which also defeats DNS rebinding). Actions need the X-Bromelia header,
/// which a page on another site can't send without Bromelia's permission (CORS).
enum WebAccess {
    static func allowed(_ r: HTTPRequest, config: WebUIConfig) -> (Bool, Int, String) {
        let token = config.token.trimmingCharacters(in: .whitespaces)
        if !token.isEmpty {
            let bearer = r.headers["authorization"].map { $0.hasPrefix("Bearer ") ? String($0.dropFirst(7)) : "" } ?? ""
            if bearer != token && r.query["token"] != token { return (false, 401, "A token is required") }
        } else {
            let host = (r.headers["host"] ?? "").lowercased()
            let name = host.hasPrefix("[") ? String(host.prefix { $0 != "]" }) + "]" : String(host.split(separator: ":").first ?? "")
            if !["localhost", "127.0.0.1", "[::1]"].contains(name) { return (false, 403, "Set a token to use Bromelia from another computer") }
        }
        if r.method == "POST" && r.headers["x-bromelia"] != "1" { return (false, 403, "Missing X-Bromelia header") }
        return (true, 200, "")
    }

    /// A page for the network (not only this computer) must have a token.
    static func startProblem(_ config: WebUIConfig) -> String? {
        let loopback = ["127.0.0.1", "localhost", "::1"].contains(config.address.trimmingCharacters(in: .whitespaces))
        if !loopback && config.token.trimmingCharacters(in: .whitespaces).isEmpty {
            return "The web page is reachable from the network only with a token. Set a token or use 127.0.0.1."
        }
        if !(1...65535).contains(config.port) { return "Port \(config.port) is not valid" }
        if config.tlsCertificate.trimmingCharacters(in: .whitespaces).isEmpty != config.tlsKey.trimmingCharacters(in: .whitespaces).isEmpty {
            return "HTTPS needs both the certificate and its key"
        }
        return nil
    }
}

/// HTTPS for the web page: the certificate and key are PEM files (as on Windows and Linux). openssl (part of macOS)
/// packs them into a PKCS #12 with a random passphrase, which is imported into memory (macOS 15) or a keychain of
/// Bromelia's own, never the login keychain.
enum WebTLS {
    static func identity(certificate: String, key: String) throws -> SecIdentity {
        let cert = Paths.expandTilde(certificate.trimmingCharacters(in: .whitespaces))
        let keyPath = Paths.expandTilde(key.trimmingCharacters(in: .whitespaces))
        let fm = FileManager.default
        for p in [cert, keyPath] where !fm.isReadableFile(atPath: p) { throw JobError.message("can't read \(p)") }
        let dir = fm.temporaryDirectory.appendingPathComponent("bromelia-tls-\(UUID().uuidString)", isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        defer { try? fm.removeItem(at: dir) }
        let p12 = dir.appendingPathComponent("identity.p12")
        let pass = UUID().uuidString
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
        // The passphrase goes through the environment, not the command line.
        p.arguments = ["pkcs12", "-export", "-in", cert, "-inkey", keyPath, "-out", p12.path, "-passout", "env:BROMELIA_P12", "-name", "Bromelia"]
        p.environment = ["BROMELIA_P12": pass]
        let err = Pipe()
        p.standardError = err
        p.standardOutput = FileHandle.nullDevice
        try p.run()
        let message = String(decoding: err.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        p.waitUntilExit()
        guard p.terminationStatus == 0, let data = try? Data(contentsOf: p12) else {
            throw JobError.message("openssl can't use the certificate and key: \(message.split(separator: "\n").first ?? "")")
        }
        var options: [String: Any] = [kSecImportExportPassphrase as String: pass]
        if #available(macOS 15.0, *) {
            options[kSecImportToMemoryOnly as String] = true
        } else {
            options[kSecImportExportKeychain as String] = try ownKeychain()
        }
        var items: CFArray?
        let status = SecPKCS12Import(data as CFData, options as CFDictionary, &items)
        guard status == errSecSuccess, let first = (items as? [[String: Any]])?.first, let id = first[kSecImportItemIdentity as String] else {
            throw JobError.message("the certificate can't be imported (\(SecCopyErrorMessageString(status, nil) as String? ?? "error \(status)"))")
        }
        return id as! SecIdentity
    }

    /// A keychain file of Bromelia's own for the imported key (before macOS 15), made again each time.
    private static func ownKeychain() throws -> SecKeychain {
        let path = Paths.appSupport.appendingPathComponent("web-tls.keychain-db").path
        try? FileManager.default.removeItem(atPath: path)
        let pass = UUID().uuidString
        var keychain: SecKeychain?
        let status = SecKeychainCreate(path, UInt32(pass.utf8.count), pass, false, nil, &keychain)
        guard status == errSecSuccess, let keychain else { throw JobError.message("can't make a keychain for the certificate (error \(status))") }
        return keychain
    }
}

/// The end of a log file for the web page: at most `limit` bytes, from the start of a line.
enum WebLog {
    static func tail(_ path: String, limit: Int = 256 * 1024) -> String {
        guard let h = FileHandle(forReadingAtPath: path) else { return "" }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        let start = size > UInt64(limit) ? size - UInt64(limit) : 0
        try? h.seek(toOffset: start)
        var data = (try? h.readToEnd()) ?? Data()
        if start > 0, let nl = data.firstIndex(of: 0x0A) { data = data[data.index(after: nl)...] }
        return (start > 0 ? "…\n" : "") + String(decoding: data, as: UTF8.self)
    }
}

/// Serves the web page (shared/web/bromelia-web.html) and its JSON API.
@MainActor
final class WebServer {
    private var listener: NWListener?
    private(set) var running: WebUIConfig?
    weak var model: AppModel?
    var lastError: String?

    init(model: AppModel) { self.model = model }

    func apply(_ config: WebUIConfig) {
        if config == running && listener != nil { return }
        stop()
        guard config.enabled else { return }
        if let p = WebAccess.startProblem(config) { lastError = p; return }
        do {
            let params: NWParameters
            if !config.tlsCertificate.trimmingCharacters(in: .whitespaces).isEmpty {
                let identity = try WebTLS.identity(certificate: config.tlsCertificate, key: config.tlsKey)
                let tls = NWProtocolTLS.Options()
                guard let secIdentity = sec_identity_create(identity) else { throw JobError.message("the certificate can't be used") }
                sec_protocol_options_set_local_identity(tls.securityProtocolOptions, secIdentity)
                params = NWParameters(tls: tls)
            } else {
                params = .tcp
            }
            params.allowLocalEndpointReuse = true
            params.requiredLocalEndpoint = .hostPort(host: NWEndpoint.Host(config.address), port: NWEndpoint.Port(integerLiteral: UInt16(config.port)))
            let l = try NWListener(using: params)
            l.newConnectionHandler = { [weak self] c in
                MainActor.assumeIsolated { self?.accept(c) }
            }
            l.stateUpdateHandler = { [weak self] state in
                if case .failed(let e) = state { MainActor.assumeIsolated { self?.lastError = "Web page: \(e.localizedDescription)" } }
            }
            l.start(queue: .main)
            listener = l
            running = config
            lastError = nil
        } catch {
            lastError = "Web page: \(error.localizedDescription)"
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
        running = nil
    }

    private func accept(_ c: NWConnection) {
        c.start(queue: .main)
        receive(c, buffer: Data())
    }

    private func receive(_ c: NWConnection, buffer: Data) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
            MainActor.assumeIsolated {
                guard let self else { c.cancel(); return }
                var buf = buffer
                if let data { buf.append(data) }
                if let r = HTTPRequest.parse(buf) {
                    self.respond(c, to: r)
                } else if done || error != nil || buf.count > 65536 {
                    c.cancel()
                } else {
                    self.receive(c, buffer: buf)
                }
            }
        }
    }

    private func respond(_ c: NWConnection, to r: HTTPRequest) {
        let (status, type, body) = handle(r)
        let reason = [200: "OK", 204: "No Content", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed"][status] ?? "Error"
        var head = "HTTP/1.1 \(status) \(reason)\r\nContent-Type: \(type)\r\nContent-Length: \(body.count)\r\nConnection: close\r\n"
        head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n"
        var out = Data(head.utf8)
        out.append(body)
        c.send(content: out, completion: .contentProcessed { _ in c.cancel() })
    }

    /// Status code, content type and body for a request.
    func handle(_ r: HTTPRequest) -> (Int, String, Data) {
        guard let model, let config = running else { return (404, "text/plain", Data()) }
        let (ok, code, why) = WebAccess.allowed(r, config: config)
        if !ok { return (code, "text/plain; charset=utf-8", Data(why.utf8)) }
        let parts = r.path.split(separator: "/").map { $0.removingPercentEncoding ?? String($0) }
        switch (r.method, parts.count) {
        case ("GET", 0):
            guard let url = Bundle.main.url(forResource: "bromelia-web", withExtension: "html"), let page = try? Data(contentsOf: url) else {
                return (404, "text/plain", Data("The page is missing from the app".utf8))
            }
            return (200, "text/html; charset=utf-8", page)
        case ("GET", 2) where parts == ["api", "status"]:
            let json = (try? JSONSerialization.data(withJSONObject: model.webStatus(), options: [.sortedKeys])) ?? Data("{}".utf8)
            return (200, "application/json", json)
        case ("GET", 4) where parts[0] == "api" && parts[1] == "jobs" && parts[3] == "log":
            guard let text = model.webLog(id: parts[2]) else { return (404, "text/plain; charset=utf-8", Data("No such job".utf8)) }
            return (200, "text/plain; charset=utf-8", Data(text.utf8))
        case ("POST", 2...4) where parts[0] == "api" && (parts.count == 4 || parts[1] == "verify"):
            // /api/<kind>/<id>/<action>, and /api/verify (check the output folder's archives) · /api/verify/cancel.
            let id = parts.count == 4 ? parts[2] : ""
            let action = parts.count == 4 ? parts[3] : parts.count == 3 ? parts[2] : "start"
            let message = model.webAction(kind: parts[1], id: id, action: action, query: r.query)
            return message == nil ? (204, "text/plain", Data()) : (400, "text/plain; charset=utf-8", Data(message!.utf8))
        default:
            return (404, "text/plain", Data("Not found".utf8))
        }
    }
}
