namespace Bromelia.Domain;

/// <summary>Messages about the drive and about MakeMKV itself that Bromelia shows outside the log.</summary>
public abstract record MakemkvNotice
{
    /// <summary>"Using LibreDrive mode (v06.3 id=…)": the drive reads the disc in LibreDrive mode.</summary>
    public sealed record LibreDrive(string Detail) : MakemkvNotice;
    /// <summary>The disc (4K UHD) can only be decrypted by a LibreDrive-compatible drive, and this one isn't.</summary>
    public sealed record LibreDriveRequired : MakemkvNotice;
    /// <summary>The evaluation period or beta key has expired (messages 5052 and 5055).</summary>
    public sealed record KeyExpired : MakemkvNotice;
    /// <summary>The evaluation hasn't been started; makemkvcon can't start it.</summary>
    public sealed record EvaluationNotStarted : MakemkvNotice;
    /// <summary>"This application version is too old": MakeMKV needs updating, or a purchased key.</summary>
    public sealed record VersionTooOld : MakemkvNotice;

    /// <summary>MakeMKV can't (fully) work until the user acts: a key, an update or starting the evaluation.</summary>
    public static bool IsLicenseProblem(MakemkvNotice notice) => notice is KeyExpired or EvaluationNotStarted or VersionTooOld;
}
