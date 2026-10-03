namespace Bromelia.Domain;

/// <summary>A decoded DVD VM jump, with its condition as text (empty when it has none).</summary>
public abstract record Jump(string Condition)
{
    /// <summary>To a chapter: of VTS title <c>TitleNumber</c> (JumpVTS_PTT) or of the current title (LinkPTTN,
    /// TitleNumber null).</summary>
    public sealed record Ptt(int? TitleNumber, int Chapter, string Condition) : Jump(Condition);
    /// <summary>To a disc title (JumpTT).</summary>
    public sealed record Title(int Number, string Condition) : Jump(Condition);
}
