using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A track (stream) of a title: its kind (attribute 1), codec (6, else 5), language (3, 4), name (2),
/// whether it's the default (<c>d</c> in 38), and every attribute MakeMKV reported.</summary>
public sealed record Track(
    int Index,
    TrackKind Kind,
    string Codec,
    string Language,
    string LanguageName,
    string Name,
    bool IsDefault,
    IReadOnlyDictionary<int, string> Attributes);
