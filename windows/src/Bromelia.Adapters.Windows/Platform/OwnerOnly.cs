using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Adapters.Windows;

/// <summary>Windows' version of 0700: a folder only the current user (and SYSTEM) can open. Nothing is inherited from
/// the folder above, so a data folder every user can read (a service's) doesn't share what is kept here; files
/// created inside inherit the same rule, so a temporary file renamed into place keeps it too.</summary>
internal static class OwnerOnly
{
    /// <summary>A folder for secrets: refused on a network share (the server decides who reads it, whatever is asked
    /// here), then made owner-only, then refused if the rule didn't hold (shared/fixtures/adapters/
    /// platform-adapters.contract.json, secrets-never-on-a-share). Fails with fs.notPrivate.</summary>
    public static void SecretsFolder(string folder)
    {
        RefuseNetwork(folder);
        Folder(folder);
        if (!Holds(folder)) throw NotPrivate(folder, "the file system didn't keep its owner-only permissions");
    }

    /// <summary>fs.notPrivate when <paramref name="path"/> is on a network share.</summary>
    public static void RefuseNetwork(string path)
    {
        if (IsOnNetworkShare(path)) throw NotPrivate(path, "it is on a network share");
    }

    /// <summary>A UNC path (\\server\share, \\?\UNC\…) or a drive Windows calls remote (a mapped share).</summary>
    public static bool IsOnNetworkShare(string path)
    {
        var full = Path.GetFullPath(path);
        if (full.StartsWith(@"\\?\UNC\", System.StringComparison.OrdinalIgnoreCase)) return true;
        if (full.StartsWith(@"\\", System.StringComparison.Ordinal) && !full.StartsWith(@"\\?\", System.StringComparison.Ordinal)
            && !full.StartsWith(@"\\.\", System.StringComparison.Ordinal)) return true;
        return Path.GetPathRoot(full) is { Length: > 0 } root && GetDriveTypeW(root) == DRIVE_REMOTE;
    }

    static BroFailure NotPrivate(string path, string reason) =>
        new(new BroMessage(MessageCode.FsNotPrivate, Severity.Error, ("path", JsonValue.Of(path)), ("reason", JsonValue.Of(reason))).ToError());

    const uint DRIVE_REMOTE = 4;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetDriveTypeW(string root);

    /// <summary>Creates <paramref name="folder"/> when needed and gives it the owner-only rule.</summary>
    public static void Folder(string folder)
    {
        var info = Directory.CreateDirectory(folder);
        var acl = new DirectorySecurity();
        acl.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        foreach (var who in Owners())
            acl.AddAccessRule(new FileSystemAccessRule(who, FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        info.SetAccessControl(acl);
    }

    /// <summary>Whether only the owners can reach <paramref name="path"/>: nothing inherited, and every rule names the
    /// current user or SYSTEM.</summary>
    public static bool Holds(string path)
    {
        FileSystemSecurity acl = Directory.Exists(path) ? new DirectoryInfo(path).GetAccessControl() : new FileInfo(path).GetAccessControl();
        if (!acl.AreAccessRulesProtected && Directory.Exists(path)) return false;
        var owners = Owners();
        foreach (FileSystemAccessRule rule in acl.GetAccessRules(true, true, typeof(SecurityIdentifier)))
            if (rule.AccessControlType == AccessControlType.Allow && System.Array.IndexOf(owners, (SecurityIdentifier)rule.IdentityReference) < 0)
                return false;
        return true;
    }

    static SecurityIdentifier[] Owners() =>
        new[] { WindowsIdentity.GetCurrent().User!, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) };
}
