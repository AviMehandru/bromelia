using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;

namespace Bromelia.Adapters.Windows;

/// <summary>Windows' version of 0700: a folder only the current user (and SYSTEM) can open. Nothing is inherited from
/// the folder above, so a data folder every user can read (a service's) doesn't share what is kept here; files
/// created inside inherit the same rule, so a temporary file renamed into place keeps it too.</summary>
internal static class OwnerOnly
{
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
