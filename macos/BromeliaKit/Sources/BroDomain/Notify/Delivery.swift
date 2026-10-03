import BroFoundation

/// How a notification goes out: an HTTP request, or an Apprise URL sent with the apprise command.
public enum Delivery: Sendable, Equatable {
    case http(request: HttpRequestSpec)
    case apprise(url: String)
}
