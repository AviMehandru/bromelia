import BroAdapters
import BroFoundation

/// Renders a message `{code, params, cause}` in English, for fixtures that check a message's text: the host's
/// MessageTextRenderer over the generated English catalog (shared/messages/en.json).
public enum English {
    private static let renderer = MessageTextRenderer(EnglishMessages.catalog)

    public static func render(_ message: JsonValue) throws -> String { renderer.renderJson(message) }
}
