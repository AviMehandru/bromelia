import BroDomain
import BroFoundation
import BroPorts

/// The apprise command (plan §10.1): `apprise -t title -b body url`. notify.needsApprise when the locator can't find
/// it (naming the URL's scheme: URLs are secrets), notify.appriseFailed when it exits non-zero.
public final class AppriseTool: Sendable {
    private let launcher: any ProcessLauncher
    private let locator: any ToolLocator

    public init(launcher: any ProcessLauncher, locator: any ToolLocator) {
        self.launcher = launcher
        self.locator = locator
    }

    public func send(_ url: String, title: String, body: String, cancel: CancellationToken) async throws(BroError) {
        guard let exe = locator.locate(.apprise).path else {
            throw BroMessage(.notifyNeedsApprise, [("target", .string(Self.scheme(url)))], severity: .warning).toError()
        }
        let exit = try await ToolRun.run(launcher, ProcessSpec(executable: exe, arguments: AppriseArgs.build(url, title: title, body: body),
                                                               environment: [:], stopPolicy: .interruptFirst, stallTimeout: Duration(seconds: 120)),
                                         cancel: cancel) { _ in nil }
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        if exit.status != 0 { throw BroMessage(.notifyAppriseFailed, [("status", .integer(Int64(exit.status)))], severity: .warning).toError() }
    }

    /// The URL's scheme ("mailto"), or the whole value when it has none (then it isn't a secret URL).
    static func scheme(_ url: String) -> String {
        guard let r = url.range(of: "://"), r.lowerBound > url.startIndex else { return url }
        return String(url[..<r.lowerBound])
    }
}
