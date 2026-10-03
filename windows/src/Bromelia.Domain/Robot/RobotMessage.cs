using System.Collections.Generic;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>A MSG line: code, flags, parameter count, MakeMKV's English text, its format and parameters.</summary>
public sealed record RobotMessage(int Code, int Flags, int Count, string Text, string Format, IReadOnlyList<string> Params)
{
    public bool Equals(RobotMessage? other) =>
        other is not null && Code == other.Code && Flags == other.Flags && Count == other.Count && Text == other.Text
        && Format == other.Format && Params.SequenceEqual(other.Params);

    public override int GetHashCode() => Code;
}
