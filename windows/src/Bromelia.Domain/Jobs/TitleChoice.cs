using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A title of an opened disc as the user left it: ticked or not, the streams kept when they were picked by
/// hand (null: MakeMKV's selection), and the output file name ("" = from the layout).</summary>
public sealed record TitleChoice(int Index, bool Selected, IReadOnlyList<int>? Tracks = null, string Name = "");
