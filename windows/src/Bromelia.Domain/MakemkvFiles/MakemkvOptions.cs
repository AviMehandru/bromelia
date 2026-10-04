namespace Bromelia.Domain;

/// <summary>The switches every makemkvcon run gets: the profile file (null: none), --minlength, --cache and
/// --directio (null: not passed), more arguments (split like a POSIX command line), and whether to scan drives
/// (false: --noscan).</summary>
public sealed record MakemkvOptions(
    string? ProfilePath = null,
    int? MinLengthSeconds = null,
    int? CacheMB = null,
    bool? DirectIO = null,
    string ExtraArguments = "",
    bool Scan = false);
