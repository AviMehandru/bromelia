import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// HttpClient on URLSession (plan §6, §9; shared/fixtures/adapters/http-client.cases.json). Any status is an answer;
/// no answer is http.failed with the system's reason; a cancelled call is job.cancelled. Follows redirects within the
/// same origin only (the same host and port; http to https on the same host too): a redirect elsewhere is the answer,
/// so a request's headers (an API key) never go to a host the request didn't name. Bodies are read up to `maxBody`
/// bytes; a larger one is http.failed. Gives up after the timeout (60 s), and says User-Agent: Bromelia unless the
/// request sets one. Errors never quote the URL: it can hold a secret.
public final class PlatformHttpClient: HttpClient {
    /// The largest answer read (16 MiB).
    public static let maxBody = 16 * 1024 * 1024

    private let session: URLSession
    private let timeout: Double

    public init(timeout: Duration? = nil) {
        self.timeout = timeout?.seconds ?? 60
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = self.timeout
        configuration.timeoutIntervalForResource = self.timeout
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: configuration)
    }

    public func send(_ request: HttpRequestSpec, cancel: CancellationToken) async throws(BroError) -> HttpResponse {
        guard let url = URL(string: request.url), let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https", url.host != nil else {
            throw failed("not a valid http(s) URL")
        }
        var r = URLRequest(url: url, timeoutInterval: timeout)
        r.httpMethod = request.method
        for h in request.headers { r.addValue(h.value, forHTTPHeaderField: h.name) }
        if !request.headers.contains(where: { $0.name.lowercased() == "user-agent" }) { r.setValue("Bromelia", forHTTPHeaderField: "User-Agent") }
        if let body = request.body { r.httpBody = Data(body) }
        let collector = Collector()
        let remove = cancel.onCancel { collector.cancel() }
        defer { remove() }
        let result: Result<HttpResponse, Error> = await withCheckedContinuation { c in
            collector.done = { c.resume(returning: $0) }
            let t = session.dataTask(with: r)
            t.delegate = collector
            collector.set(t)
            if cancel.isCancelled { t.cancel() }
            t.resume()
        }
        switch result {
        case .success(let response): return response
        case .failure(let error as BroError): throw error
        case .failure(let error):
            if cancel.isCancelled || (error as? URLError)?.code == .cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
            throw failed(error.localizedDescription)
        }
    }

    /// Whether a redirect from `from` may go to `to`: the same host and port, or http to https on the same host with the
    /// default ports.
    static func sameOrigin(_ from: URL, _ to: URL) -> Bool {
        guard let a = from.host?.lowercased(), let b = to.host?.lowercased(), a == b else { return false }
        let fromScheme = from.scheme?.lowercased(), toScheme = to.scheme?.lowercased()
        func port(_ u: URL, _ scheme: String?) -> Int? { u.port ?? (scheme == "http" ? 80 : scheme == "https" ? 443 : nil) }
        if fromScheme == toScheme { return port(from, fromScheme) == port(to, toScheme) }
        return fromScheme == "http" && toScheme == "https" && port(from, fromScheme) == 80 && port(to, toScheme) == 443
    }

    private func failed(_ reason: String) -> BroError {
        BroMessage(.httpFailed, [("reason", .string(reason))], severity: .error).toError()
    }

    /// One call: decides each redirect, collects the body up to maxBody, and reports the end once.
    private final class Collector: NSObject, URLSessionDataDelegate, @unchecked Sendable {
        private let lock = NSLock()
        private var task: URLSessionTask?
        private var cancelled = false
        private var body: [UInt8] = []
        private var tooBig = false
        var done: ((Result<HttpResponse, Error>) -> Void)?

        func set(_ t: URLSessionTask) { lock.withLock { task = t; if cancelled { t.cancel() } } }
        func cancel() { lock.withLock { cancelled = true; task?.cancel() } }

        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
            guard let from = task.currentRequest?.url, let to = request.url, PlatformHttpClient.sameOrigin(from, to) else {
                completionHandler(nil)   // the redirect itself is the answer
                return
            }
            completionHandler(request)
        }

        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
            let over = lock.withLock { () -> Bool in
                if body.count + data.count > PlatformHttpClient.maxBody { tooBig = true; return true }
                body.append(contentsOf: data)
                return false
            }
            if over { dataTask.cancel() }
        }

        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
            let (bytes, big) = lock.withLock { (body, tooBig) }
            let result: Result<HttpResponse, Error>
            if big {
                result = .failure(BroMessage(.httpFailed, [("reason", .string("the answer is larger than \(PlatformHttpClient.maxBody / 1024 / 1024) MiB"))],
                                             severity: .error).toError())
            } else if let error {
                result = .failure(error)
            } else {
                let http = task.response as? HTTPURLResponse
                let headers = (http?.allHeaderFields ?? [:]).map { (name: "\($0.key)", value: "\($0.value)") }
                    .sorted { $0.name.lowercased() < $1.name.lowercased() }
                result = .success(HttpResponse(status: http?.statusCode ?? 0, headers: headers, body: bytes))
            }
            let finish = lock.withLock { () -> ((Result<HttpResponse, Error>) -> Void)? in
                defer { done = nil }
                return done
            }
            finish?(result)
        }
    }
}
