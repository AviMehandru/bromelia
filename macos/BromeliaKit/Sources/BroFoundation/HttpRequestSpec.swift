/// A request built by Domain and sent by the HttpClient port: method, URL, headers in order, body.
public struct HttpRequestSpec: Sendable, Equatable {
    public var method: String
    public var url: String
    public var headers: [(name: String, value: String)]
    public var body: [UInt8]?

    public init(method: String, url: String, headers: [(name: String, value: String)] = [], body: [UInt8]? = nil) {
        self.method = method
        self.url = url
        self.headers = headers
        self.body = body
    }

    public static func == (a: HttpRequestSpec, b: HttpRequestSpec) -> Bool {
        a.method == b.method && a.url == b.url && a.body == b.body
            && a.headers.count == b.headers.count && zip(a.headers, b.headers).allSatisfy { $0 == $1 }
    }
}
