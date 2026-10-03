namespace Bromelia.Domain;

/// <summary>A checked archive folder and its result.</summary>
public sealed record FolderCheck(string Folder, VerifyResult Result);
