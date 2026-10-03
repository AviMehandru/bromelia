namespace Bromelia.Domain;

/// <summary>A menu still: the sectors [first, end) of one VOB/cell id in a menu VOB (input for MenuOcr).</summary>
public sealed record CellRef(string File, long FirstSector, long EndSector);
