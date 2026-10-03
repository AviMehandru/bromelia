using System.Collections.Generic;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Configuration documents, version 3: decoding fills defaults outside profiles, writes keys in schema
/// order and reports (and drops) unknown keys; encoding is canonical JSON.</summary>
public static class ConfigCodec
{
    /// <summary>A version 3 document. Throws <see cref="BroFailure"/> (config.rejected) when the bytes aren't a
    /// JSON object; other problems are issues (see <see cref="ConfigValidator.Validate"/>).</summary>
    public static Config Decode(byte[] bytes)
    {
        if (JsonValue.Parse(bytes) is not JsonValue.Object doc)
            throw new BroFailure(new BroMessage(MessageCode.ConfigRejected, Severity.Error, ("count", JsonValue.Of(1))).ToError());
        var issues = new List<Issue>();
        var normalized = SchemaWalker.Normalize(doc, SchemaWalker.Document, "", true, issues);
        return new Config(normalized, issues);
    }

    /// <summary>Two-space indented JSON with a final newline, keys in schema order.</summary>
    public static byte[] Encode(Config config) => JsonValue.EncodeCanonical(config.Document);

    /// <summary>{format: bromelia-config, version: 3, config}: the configuration without secrets (SecretRefs are
    /// kept; their values never are in the document).</summary>
    public static byte[] ExportBundle(Config config) => JsonValue.EncodeCanonical(JsonValue.Of(
        ("format", JsonValue.Of("bromelia-config")), ("version", JsonValue.Of(3)), ("config", config.Document)));
}
