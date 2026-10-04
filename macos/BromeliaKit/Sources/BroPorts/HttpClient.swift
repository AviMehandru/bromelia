import BroDomain
import BroFoundation

/// Sends requests that Domain built.
public protocol HttpClient: Sendable {
    /// Any status is an answer; fails when there is none.
    func send(_ request: HttpRequestSpec, cancel: CancellationToken) async throws(BroError) -> HttpResponse
}
