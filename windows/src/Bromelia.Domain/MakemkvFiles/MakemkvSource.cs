namespace Bromelia.Domain;

/// <summary>A source makemkvcon can open: a drive (MakeMKV's index and the OS device, "" when unknown), a disc image,
/// or a folder holding a disc structure.</summary>
public abstract record MakemkvSource
{
    public sealed record Drive(int Index, string Device) : MakemkvSource;
    public sealed record Iso(string Path) : MakemkvSource;
    public sealed record File(string Path) : MakemkvSource;
}
