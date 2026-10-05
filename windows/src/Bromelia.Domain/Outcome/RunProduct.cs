namespace Bromelia.Domain;

/// <summary>What a makemkvcon run should leave in its destination: nothing (a listing), titles (MKV files of a rip) or
/// a backup (a disc structure or an ISO image).</summary>
public enum RunProduct
{
    Nothing,
    Titles,
    Backup,
}
