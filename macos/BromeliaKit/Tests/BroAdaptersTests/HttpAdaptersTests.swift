import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Darwin
import Foundation
import Testing

/// A tiny HTTP/1.1 server on 127.0.0.1 with the routes of http-client.cases.json: /echo, /status/n, /slow.
final class TestHttpServer: @unchecked Sendable {
    let fd: Int32
    let port: UInt16

    init() {
        let s = socket(AF_INET, SOCK_STREAM, 0)
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        addr.sin_port = 0
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = bind(s, $0, len) }
        }
        listen(s, 16)
        withUnsafeMutablePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = getsockname(s, $0, &len) }
        }
        fd = s
        port = UInt16(bigEndian: addr.sin_port)
        Thread.detachNewThread { [self] in loop() }
    }

    var url: String { "http://127.0.0.1:\(port)" }

    /// A port nothing listens on.
    static func closed() -> String {
        let s = TestHttpServer()
        let u = s.url
        s.stop()
        return u
    }

    func stop() { close(fd) }

    private func loop() {
        while true {
            let c = accept(fd, nil, nil)
            if c < 0 { return }
            Thread.detachNewThread { Self.serve(c) }
        }
    }

    private static func serve(_ c: Int32) {
        defer { close(c) }
        var head: [UInt8] = []
        var byte: UInt8 = 0
        while !(head.count >= 4 && head.suffix(4) == [13, 10, 13, 10]) {
            if read(c, &byte, 1) != 1 { return }
            head.append(byte)
        }
        let lines = String(decoding: head, as: UTF8.self).components(separatedBy: "\r\n")
        let parts = lines[0].split(separator: " ").map(String.init)
        var header: String?
        var length = 0
        for l in lines.dropFirst() {
            guard let i = l.firstIndex(of: ":") else { continue }
            let name = l[..<i].trimmingCharacters(in: .whitespaces).lowercased(), value = l[l.index(after: i)...].trimmingCharacters(in: .whitespaces)
            if name == "x-test" { header = value }
            if name == "content-length" { length = Int(value) ?? 0 }
        }
        var body = [UInt8](repeating: 0, count: length)
        var got = 0
        while got < length {
            let n = body.withUnsafeMutableBytes { read(c, $0.baseAddress! + got, length - got) }
            if n <= 0 { break }
            got += n
        }
        func json(_ s: String) -> String { String(decoding: JsonValue.encodeCanonical(.string(s)), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines) }
        var status = 200
        var text: String
        if parts[1] == "/echo" {
            text = "{\"method\":\(json(parts[0])),\"path\":\"/echo\",\"header\":\(header.map(json) ?? "null"),\"body\":\(json(String(decoding: body, as: UTF8.self)))}"
        } else if parts[1].hasPrefix("/status/") {
            status = Int(parts[1].dropFirst("/status/".count)) ?? 500
            text = "status \(status)"
        } else {
            Thread.sleep(forTimeInterval: 5)
            text = "slow"
        }
        let bytes = Array(text.utf8)
        let response = Array("HTTP/1.1 \(status) X\r\nContent-Length: \(bytes.count)\r\nConnection: close\r\n\r\n".utf8) + bytes
        _ = response.withUnsafeBytes { write(c, $0.baseAddress, $0.count) }
    }
}

/// An HttpClient that records the request and gives a fixed answer (a status and body, or no answer).
final class FakeHttp: HttpClient, @unchecked Sendable {
    private let lock = NSLock()
    private var sentRequest: HttpRequestSpec?
    var status = 200
    var body: [UInt8] = []
    var failReason: String?
    var sent: HttpRequestSpec? { lock.withLock { sentRequest } }

    func send(_ request: HttpRequestSpec, cancel: CancellationToken) async throws(BroError) -> HttpResponse {
        lock.withLock { sentRequest = request }
        if let failReason { throw BroError("http.failed", [("reason", .string(failReason))]) }
        return HttpResponse(status: status, headers: [], body: body)
    }
}

/// Finds the tools it is given.
struct MapLocator: ToolLocator {
    let paths: [ToolKind: String]
    func locate(_ tool: ToolKind) -> ToolInfo {
        if let p = paths[tool] { return ToolInfo(tool: tool, path: p, capabilities: []) }
        return ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning))
    }
}

/// http-client.cases.json, notification-sender.cases.json and beta-key-source.cases.json.
@Suite(.serialized) struct HttpAdaptersTests {
    func sameError(_ want: JsonValue, _ e: BroError) throws {
        try Fixtures.same(want["code"]?.string, e.code, "code")
        for (k, v) in want["params"]?.members ?? [] { try Fixtures.same(v, JsonValue.object(e.params)[k] ?? .null, k) }
    }

    @Test func theHttpClientCasesPass() async throws {
        let server = TestHttpServer()
        defer { server.stop() }
        let closed = TestHttpServer.closed()
        let doc = try Fixtures.json("adapters/http-client.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                let url = given["url"]!.string!.replacingOccurrences(of: "<server>", with: server.url).replacingOccurrences(of: "<closed>", with: closed)
                let request = HttpRequestSpec(method: given["method"]!.string!, url: url,
                                              headers: (given["headers"]?.members ?? []).map { (name: $0.key, value: $0.value.string!) },
                                              body: given["body"]?.string.map { Array($0.utf8) })
                let cancelSource = CancellationSource()
                let cancel = cancelSource.token
                if let after = given["cancelAfter"]?.double {
                    Task { try? await Task.sleep(nanoseconds: UInt64(after * 1e9)); cancelSource.cancel() }
                }
                let start = Date()
                do {
                    let response = try await PlatformHttpClient().send(request, cancel: cancel)
                    try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                    try Fixtures.same(expect["status"]?.int, Int64(response.status), "status")
                    if let json = expect["json"] { try Fixtures.same(json, JsonValue.parse(response.body) ?? .null, "json") }
                    if let text = expect["text"]?.string { try Fixtures.same(text, String(decoding: response.body, as: UTF8.self), "text") }
                } catch let error as BroError {
                    try Fixtures.same(expect["error"]?.string, error.code, "error")
                }
                if let within = expect["within"]?.double { try Fixtures.check(Date().timeIntervalSince(start) < within, "within \(within) s") }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theNotificationCasesPass() async throws {
        let doc = try Fixtures.json("adapters/notification-sender.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                let http = FakeHttp()
                if let answer = given["answer"] {
                    http.status = Int(answer["status"]?.int ?? 200)
                    http.failReason = answer["reason"]?.string
                }
                let launcher = ScriptedProcessLauncher()
                launcher.exitCode = Int(given["appriseExit"]?.int ?? 0)
                let tools: [ToolKind: String] = given["apprise"]?.isNull == true ? [:] : [.apprise: "/opt/apprise"]
                let sender = NotificationSender(http: http, apprise: AppriseTool(launcher: launcher, locator: MapLocator(paths: tools)))
                do {
                    try await sender.send(given["url"]!.string!, title: "T", body: "B", status: StatusWord(rawValue: given["status"]!.string!)!,
                                          cancel: CancellationSource().token)
                    try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
                } catch let error as BroError {
                    guard let want = expect["error"] else { throw error }
                    try sameError(want, error)
                }
                if let want = expect["request"] {
                    try Fixtures.same(want["method"]?.string, http.sent?.method, "method")
                    try Fixtures.same(want["url"]?.string, http.sent?.url, "url")
                    if let json = want["json"] { try Fixtures.same(json, JsonValue.parse(http.sent?.body ?? []) ?? .null, "json") }
                    if let text = want["text"]?.string { try Fixtures.same(text, String(decoding: http.sent?.body ?? [], as: UTF8.self), "text") }
                }
                if let argv = expect["apprise"]?.array {
                    let spec = launcher.started.first
                    try Fixtures.same(argv.map { $0.string! }, spec.map { [$0.executable] + $0.arguments }, "apprise")
                    let env = (expect["appriseEnvironment"]?.members ?? []).map { "\($0.key)=\($0.value.string!)" }
                    try Fixtures.same(env, (spec?.environment ?? [:]).sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }, "appriseEnvironment")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theBetaKeyCasesPass() async throws {
        let doc = try Fixtures.json("adapters/beta-key-source.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let answer = c["given"]!["answer"]!, expect = c["expect"]!
            do {
                let http = FakeHttp()
                http.status = Int(answer["status"]?.int ?? 200)
                http.failReason = answer["reason"]?.string
                http.body = try answer["file"]?.string.map { try Fixtures.bytes($0) } ?? Array((answer["text"]?.string ?? "").utf8)
                do {
                    let key = try await BetaKeySource(http: http).currentKey(CancellationSource().token)
                    try Fixtures.check(key.hasPrefix(expect["keyPrefix"]!.string!), "key \(key)")
                    try Fixtures.same(BetaKeyPage.url, http.sent?.url, "url")
                    try Fixtures.same("GET", http.sent?.method, "method")
                } catch let error as BroError {
                    guard let want = expect["error"] else { throw error }
                    try sameError(want, error)
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
