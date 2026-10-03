namespace Bromelia.Domain;

/// <summary>What a disc label such as ONE_PIECE_S2_P7_D2 says: the title, the place in a set, and whether it
/// looks like part of a series.</summary>
public sealed record Label(string Title, int? Season = null, int? Part = null, int? Volume = null, int? Disc = null, bool LooksLikeSeries = false);
