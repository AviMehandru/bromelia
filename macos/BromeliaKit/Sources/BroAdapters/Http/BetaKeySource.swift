import BroDomain
import BroFoundation
import BroPorts

/// MakeMKV's free beta key (plan §10.2; shared/fixtures/adapters/beta-key-source.cases.json): the forum post
/// (BetaKeyPage.url), read with BetaKeyPage.parse. makemkv.betaKey.http for another status, .notFound when the page
/// has no key, .fetchFailed (with the reason) when there is no answer.
public final class BetaKeySource: Sendable {
    private let http: any HttpClient

    public init(http: any HttpClient) { self.http = http }

    public func currentKey(_ cancel: CancellationToken) async throws(BroError) -> String {
        let response: HttpResponse
        do {
            response = try await http.send(HttpRequestSpec(method: "GET", url: BetaKeyPage.url), cancel: cancel)
        } catch where error.code == MessageCode.httpFailed.rawValue {
            let reason = JsonValue.object([("code", .string(error.code)), ("params", .object(error.params))])
            throw BroMessage(.makemkvBetaKeyFetchFailed, [("reason", reason)], severity: .error).toError()
        }
        guard response.status == 200 else {
            throw BroMessage(.makemkvBetaKeyHttp, [("status", .integer(Int64(response.status)))], severity: .error).toError()
        }
        guard let key = BetaKeyPage.parse(String(decoding: response.body, as: UTF8.self)) else {
            throw BroMessage(.makemkvBetaKeyNotFound, severity: .error).toError()
        }
        return key
    }
}
