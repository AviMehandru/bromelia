import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// HttpClient on URLSession (plan §6, §9; shared/fixtures/adapters/http-client.cases.json). Any status is an answer;
/// no answer is http.failed with the system's reason; a cancelled call is job.cancelled. Follows redirects, gives up
/// after the timeout (60 s), and says User-Agent: Bromelia unless the request sets one.
public final class PlatformHttpClient: HttpClient {
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
        guard let url = URL(string: request.url) else { throw failed("not a URL: \(request.url)") }
        var r = URLRequest(url: url, timeoutInterval: timeout)
        r.httpMethod = request.method
        for h in request.headers { r.addValue(h.value, forHTTPHeaderField: h.name) }
        if !request.headers.contains(where: { $0.name.lowercased() == "user-agent" }) { r.setValue("Bromelia", forHTTPHeaderField: "User-Agent") }
        if let body = request.body { r.httpBody = Data(body) }
        let task = Box()
        let remove = cancel.onCancel { task.cancel() }
        defer { remove() }
        let result: Result<(Data, URLResponse), Error> = await withCheckedContinuation { c in
            let t = session.dataTask(with: r) { data, response, error in
                if let error { c.resume(returning: .failure(error)) } else { c.resume(returning: .success((data ?? Data(), response!))) }
            }
            task.set(t)
            if cancel.isCancelled { t.cancel() }
            t.resume()
        }
        switch result {
        case .success(let (data, response)):
            let http = response as? HTTPURLResponse
            let headers = (http?.allHeaderFields ?? [:]).map { (name: "\($0.key)", value: "\($0.value)") }.sorted { $0.name.lowercased() < $1.name.lowercased() }
            return HttpResponse(status: http?.statusCode ?? 0, headers: headers, body: Array(data))
        case .failure(let error):
            if cancel.isCancelled || (error as? URLError)?.code == .cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
            throw failed(error.localizedDescription)
        }
    }

    private func failed(_ reason: String) -> BroError {
        BroMessage(.httpFailed, [("reason", .string(reason))], severity: .error).toError()
    }

    /// The running task, for cancellation from another thread.
    private final class Box: @unchecked Sendable {
        private let lock = NSLock()
        private var task: URLSessionTask?
        private var cancelled = false
        func set(_ t: URLSessionTask) { lock.withLock { task = t; if cancelled { t.cancel() } } }
        func cancel() { lock.withLock { cancelled = true; task?.cancel() } }
    }
}
