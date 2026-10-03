namespace Bromelia.Domain;

/// <summary>How a chapter of a title is reached (the first way found): a menu, a button or a pre-command.</summary>
public sealed record NavJump(int Title, int Chapter, string How);
