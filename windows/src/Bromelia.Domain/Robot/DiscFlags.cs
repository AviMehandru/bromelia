namespace Bromelia.Domain;

/// <summary>The disc flags of a DRV line: which file systems MakeMKV found.</summary>
public readonly record struct DiscFlags(int Raw)
{
    public bool DvdFiles => (Raw & 1) != 0;
    public bool HdDvdFiles => (Raw & 2) != 0;
    public bool BlurayFiles => (Raw & 4) != 0;
    public bool AacsFiles => (Raw & 8) != 0;
    public bool BdsvmFiles => (Raw & 16) != 0;

    /// <summary>MakeMKV's disc type: <c>Blu-ray (AACS)</c>, <c>Blu-ray</c>, <c>HD DVD</c>, <c>DVD</c> or <c>Disc</c>.</summary>
    public static string TypeText(DiscFlags flags)
    {
        if (flags.BlurayFiles) return flags.AacsFiles ? "Blu-ray (AACS)" : "Blu-ray";
        if (flags.HdDvdFiles) return "HD DVD";
        if (flags.DvdFiles) return "DVD";
        return "Disc";
    }
}
