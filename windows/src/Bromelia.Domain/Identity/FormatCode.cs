namespace Bromelia.Domain;

/// <summary>The format code of file names: DVD, BR, 4K, HDDVD or DISC, with an <c>e</c> suffix for backups that
/// weren't decrypted (common.json's FormatCode).</summary>
public readonly record struct FormatCode(string Text)
{
    public override string ToString() => Text;
}
