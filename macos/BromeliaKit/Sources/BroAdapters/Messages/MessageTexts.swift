/// One language's message texts (ICU MessageFormat, shared/messages/<lang>.json) by code, and the params codes.json
/// declares as messageList (rendered joined with ", "; other message lists with "; ").
public struct MessageTexts: Sendable {
    private let texts: [String: String]
    private let messageLists: Set<String>

    public init(texts: [String: String], messageLists: [(code: String, param: String)]) {
        self.texts = texts
        self.messageLists = Set(messageLists.map { $0.code + "\u{0}" + $0.param })
    }

    /// The text of a code; nil when the catalog has none.
    public func text(_ code: String) -> String? { texts[code] }

    /// Whether a code's param is a messageList.
    public func isMessageList(_ code: String, param: String) -> Bool { messageLists.contains(code + "\u{0}" + param) }
}
