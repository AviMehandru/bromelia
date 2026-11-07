using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The .bromelia-library file at a library's root (plan §22.2): written when a library is attached (never on
/// its own), so a library whose disk isn't there is offline instead of being written to the boot disk.</summary>
public sealed record LibraryMarker(string Id, string Name, Instant? Created)
{
    public const string FileName = ".bromelia-library";

    /// <summary>A marker for a library attached now. Its id is a UUID (version 8, RFC 9562) made from the first 16
    /// bytes of SHA-256 of "&lt;libraryId&gt;\n&lt;path&gt;\n&lt;created&gt;": unique to this attach without any
    /// randomness.</summary>
    public static LibraryMarker Make(string libraryId, string path, string name, Instant created)
    {
        var b = SHA256.HashData(Encoding.UTF8.GetBytes(libraryId + "\n" + path + "\n" + Instant.Format(created)));
        b[6] = (byte)((b[6] & 0x0F) | 0x80);
        b[8] = (byte)((b[8] & 0x3F) | 0x80);
        var hex = System.Convert.ToHexString(b, 0, 16).ToLowerInvariant();
        var id = hex[..8] + "-" + hex[8..12] + "-" + hex[12..16] + "-" + hex[16..20] + "-" + hex[20..32];
        return new LibraryMarker(id, name, created);
    }

    /// <summary>{"id", "name", "created"}, canonical JSON with a final newline.</summary>
    public static byte[] Encode(LibraryMarker marker)
    {
        var m = new List<KeyValuePair<string, JsonValue>> { new("id", JsonValue.Of(marker.Id)), new("name", JsonValue.Of(marker.Name)) };
        if (marker.Created is { } c) m.Add(new("created", JsonValue.Of(Instant.Format(c))));
        return JsonValue.EncodeCanonical(new JsonValue.Object(m));
    }

    /// <summary>Null unless a JSON object with a string id; the name defaults to "" and created to null.</summary>
    public static LibraryMarker? Decode(byte[] bytes)
    {
        var json = JsonValue.Parse(bytes);
        if (json is not JsonValue.Object || json["id"]?.AsString is not { } id) return null;
        return new LibraryMarker(id, json["name"]?.AsString ?? "", json["created"]?.AsString is { } c ? Instant.Parse(c) : null);
    }
}
