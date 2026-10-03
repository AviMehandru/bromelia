import BroFoundation

/// Configuration documents, version 3: decoding fills defaults outside profiles, writes keys in schema order and
/// reports (and drops) unknown keys; encoding is canonical JSON.
public enum ConfigCodec {
    /// A version 3 document. Throws config.rejected when the bytes aren't a JSON object; other problems are issues
    /// (see `ConfigValidator.validate`).
    public static func decode(_ bytes: [UInt8]) throws(BroError) -> Config {
        guard let doc = JsonValue.parse(bytes), case .object = doc else {
            throw BroMessage(.configRejected, [("count", .integer(1))], severity: .error).toError()
        }
        var issues: [Issue] = []
        let normalized = SchemaWalker.normalize(doc, SchemaWalker.document, "", fill: true, &issues)
        return Config(document: normalized, issues: issues)
    }

    /// Two-space indented JSON with a final newline, keys in schema order.
    public static func encode(_ config: Config) -> [UInt8] { JsonValue.encodeCanonical(config.document) }

    /// {format: bromelia-config, version: 3, config}: the configuration without secrets (SecretRefs are kept; their
    /// values never are in the document).
    public static func exportBundle(_ config: Config) -> [UInt8] {
        JsonValue.encodeCanonical(.object([("format", .string("bromelia-config")), ("version", .integer(3)), ("config", config.document)]))
    }
}
