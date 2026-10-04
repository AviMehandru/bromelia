import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Notifications (plan §10.2; shared/fixtures/adapters/notification-sender.cases.json): NotifyRequests.build, then the
/// HttpClient or Apprise. A failure is notify.badUrl, notify.httpFailed {host, status}, notify.failed {host, reason},
/// notify.needsApprise or notify.appriseFailed; none names the URL, which is a secret.
public final class NotificationSender: Sendable {
    private let http: any HttpClient
    private let apprise: AppriseTool

    public init(http: any HttpClient, apprise: AppriseTool) {
        self.http = http
        self.apprise = apprise
    }

    public func send(_ url: String, title: String, body: String, status: StatusWord, cancel: CancellationToken) async throws(BroError) {
        switch NotifyRequests.build(url, title: title, body: body, status: status) {
        case .http(let request):
            let host = URL(string: request.url)?.host ?? ""
            let response: HttpResponse
            do {
                response = try await http.send(request, cancel: cancel)
            } catch where error.code == MessageCode.httpFailed.rawValue {
                let reason = JsonValue.object(error.params)["reason"] ?? .string("")
                throw BroMessage(.notifyFailed, [("host", .string(host)), ("reason", reason)], severity: .warning).toError()
            }
            if response.status < 200 || response.status >= 300 {
                throw BroMessage(.notifyHttpFailed, [("host", .string(host)), ("status", .integer(Int64(response.status)))], severity: .warning).toError()
            }
        case .apprise(let appriseUrl):
            try await apprise.send(appriseUrl, title: title, body: body, cancel: cancel)
        case nil:
            throw BroMessage(.notifyBadUrl, [("target", .string(AppriseTool.scheme(url)))], severity: .warning).toError()
        }
    }
}
