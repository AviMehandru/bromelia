import BroDomain
import BroFoundation

/// An HTTP answer: status, headers in order, body.
public struct HttpResponse: Sendable, Equatable {
    public var status: Int
    public var headers: [(name: String, value: String)]
    public var body: [UInt8]

    public init(status: Int, headers: [(name: String, value: String)], body: [UInt8]) {
        self.status = status
        self.headers = headers
        self.body = body
    }

    public static func == (a: HttpResponse, b: HttpResponse) -> Bool {
        a.status == b.status && a.headers.map { [$0.name, $0.value] } == b.headers.map { [$0.name, $0.value] } && a.body == b.body
    }
}
