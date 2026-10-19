using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Whether a backup a job made looks like a disc (BackupStructure.problem), read from disk: a folder by its
/// files, an image by its volume descriptor at byte 32769.</summary>
public static class BackupCheck
{
    /// <summary>What is wrong with the backup at <paramref name="path"/> (named by its last part), or null:
    /// structure.notCreated when nothing is there, structure.unreadable when it can't be read.</summary>
    public static BroMessage? Problem(IFileSystem fs, string path)
    {
        var name = path.Substring(path.LastIndexOf('/') + 1);
        if (!fs.Exists(path)) return BackupStructure.Problem(name, false, null, null);
        try
        {
            return fs.Stat(path).IsDirectory
                ? BackupStructure.Problem(name, false, FileTree.Files(fs, path), null)
                : BackupStructure.Problem(name, true, null, fs.ReadRange(path, 32769, 5));
        }
        catch (BroFailure)
        {
            return new BroMessage(MessageCode.StructureUnreadable, Severity.Info, ("name", JsonValue.Of(name)));
        }
    }
}
