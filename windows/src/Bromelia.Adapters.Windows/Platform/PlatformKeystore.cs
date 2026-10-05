using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters.Windows;

/// <summary>Keystore on Windows (plan §10.3): generic credentials in Credential Manager, target "&lt;service&gt;:&lt;name&gt;",
/// the value as UTF-8. When Credential Manager can't be used at all (no logon session: a service account, an SSH key
/// logon), the secrets are files &lt;fallbackDir&gt;/&lt;name&gt; instead; that is decided once, at the first call. The folder
/// is owner-only (Windows' 0700: the current user and SYSTEM, nothing inherited), since a service's data folder may be
/// readable by every user; files written there inherit that. Names are SecretRef names (keystore.cases.json); anything
/// else fails with keystore.failed.</summary>
public sealed class PlatformKeystore : IKeystore
{
    const uint CRED_TYPE_GENERIC = 1;
    const uint CRED_PERSIST_LOCAL_MACHINE = 2;
    const int CRED_MAX_CREDENTIAL_BLOB_SIZE = 5 * 512;
    const int ERROR_NOT_FOUND = 1168;
    const int ERROR_NO_SUCH_LOGON_SESSION = 1312;

    readonly string _fallbackDir;
    readonly string _service;
    readonly object _gate = new();
    bool? _system;

    /// <param name="fallbackDir">Where the secrets go when Credential Manager can't be used.</param>
    /// <param name="service">The target prefix (tests use their own).</param>
    /// <param name="systemStore">False: always the files (tests).</param>
    public PlatformKeystore(string fallbackDir, string service = "Bromelia", bool systemStore = true)
    {
        _fallbackDir = fallbackDir;
        _service = service;
        if (!systemStore) _system = false;
    }

    /// <summary>Where the secrets are: "credentialManager" or "file".</summary>
    public string Backend() => UseSystem() ? "credentialManager" : "file";

    public string? Get(string name)
    {
        Check(name);
        if (!UseSystem()) return ReadFile(name);
        if (!CredReadW(Target(name), CRED_TYPE_GENERIC, 0, out var handle))
        {
            var code = Marshal.GetLastWin32Error();
            if (code == ERROR_NOT_FOUND) return null;
            throw Failed(name, new Win32Exception(code).Message);
        }
        try
        {
            var cred = Marshal.PtrToStructure<CREDENTIAL>(handle);
            var bytes = new byte[cred.CredentialBlobSize];
            if (bytes.Length > 0) Marshal.Copy(cred.CredentialBlob, bytes, 0, bytes.Length);
            return Encoding.UTF8.GetString(bytes);
        }
        finally { CredFree(handle); }
    }

    public void Set(string name, string value)
    {
        Check(name);
        if (!UseSystem())
        {
            WriteFile(name, value);
            return;
        }
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length > CRED_MAX_CREDENTIAL_BLOB_SIZE)
            throw Failed(name, $"longer than Credential Manager keeps ({CRED_MAX_CREDENTIAL_BLOB_SIZE} bytes)");
        var blob = Marshal.AllocHGlobal(Math.Max(bytes.Length, 1));
        try
        {
            Marshal.Copy(bytes, 0, blob, bytes.Length);
            var cred = new CREDENTIAL
            {
                Type = CRED_TYPE_GENERIC,
                TargetName = Target(name),
                CredentialBlobSize = (uint)bytes.Length,
                CredentialBlob = blob,
                Persist = CRED_PERSIST_LOCAL_MACHINE,
                UserName = _service,
            };
            if (!CredWriteW(ref cred, 0)) throw Failed(name, new Win32Exception(Marshal.GetLastWin32Error()).Message);
        }
        finally
        {
            Marshal.Copy(new byte[bytes.Length], 0, blob, bytes.Length);
            Marshal.FreeHGlobal(blob);
        }
    }

    public void Remove(string name)
    {
        Check(name);
        if (!UseSystem())
        {
            try { if (File.Exists(FilePath(name))) File.Delete(FilePath(name)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { throw Failed(name, e.Message); }
            return;
        }
        if (!CredDeleteW(Target(name), CRED_TYPE_GENERIC, 0))
        {
            var code = Marshal.GetLastWin32Error();
            if (code != ERROR_NOT_FOUND) throw Failed(name, new Win32Exception(code).Message);
        }
    }

    /// <summary>Credential Manager, unless it has no logon session to keep credentials in (asked once).</summary>
    bool UseSystem()
    {
        lock (_gate)
        {
            if (_system is { } known) return known;
            var usable = true;
            if (CredReadW(Target("bromelia.probe"), CRED_TYPE_GENERIC, 0, out var handle)) CredFree(handle);
            else if (Marshal.GetLastWin32Error() == ERROR_NO_SUCH_LOGON_SESSION) usable = false;
            _system = usable;
            return usable;
        }
    }

    string Target(string name) => $"{_service}:{name}";

    string FilePath(string name) => Path.Combine(_fallbackDir, name);

    string? ReadFile(string name)
    {
        try { return File.Exists(FilePath(name)) ? Encoding.UTF8.GetString(File.ReadAllBytes(FilePath(name))) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { throw Failed(name, e.Message); }
    }

    /// <summary>Written next to the final file, then moved over it: a reader sees the old secret or the new one.</summary>
    void WriteFile(string name, string value)
    {
        var temp = Path.Combine(_fallbackDir, $".{name}.{Guid.NewGuid():N}.tmp");
        try
        {
            OwnerOnly.Folder(_fallbackDir); // before the secret is written, so the new file inherits it
            File.WriteAllBytes(temp, Encoding.UTF8.GetBytes(value));
            File.Move(temp, FilePath(name), overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.AccessControl.PrivilegeNotHeldException)
        {
            try { File.Delete(temp); } catch (IOException) { }
            throw Failed(name, e.Message);
        }
    }

    /// <summary>A SecretRef name: ^[a-z][a-zA-Z0-9]*(\.[A-Za-z0-9_-]+)*$.</summary>
    static bool IsSecretName(string name)
    {
        if (name.Length == 0 || name[0] is < 'a' or > 'z') return false;
        var i = 1;
        while (i < name.Length && char.IsAsciiLetterOrDigit(name[i])) i++;
        while (i < name.Length)
        {
            if (name[i] != '.') return false;
            var start = ++i;
            while (i < name.Length && (char.IsAsciiLetterOrDigit(name[i]) || name[i] is '_' or '-')) i++;
            if (i == start) return false;
        }
        return true;
    }

    static void Check(string name)
    {
        if (!IsSecretName(name)) throw Failed(name, "not a secret name");
    }

    static BroFailure Failed(string name, string reason) =>
        new(new BroMessage(MessageCode.KeystoreFailed, Severity.Error, ("name", JsonValue.Of(name)), ("reason", JsonValue.Of(reason))).ToError());

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct CREDENTIAL
    {
        public uint Flags;
        public uint Type;
        public string TargetName;
        public string? Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public string? TargetAlias;
        public string? UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CredReadW(string target, uint type, uint flags, out IntPtr credential);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CredWriteW(ref CREDENTIAL credential, uint flags);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CredDeleteW(string target, uint type, uint flags);

    [DllImport("advapi32.dll")]
    static extern void CredFree(IntPtr buffer);
}
