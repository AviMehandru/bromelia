using Bromelia.Ports;
using PortFileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Adapters.Tests;

/// <summary>The few FileSystem calls the portable adapters' tests need, on System.IO (the platform adapter lives in
/// the Windows project).</summary>
internal sealed class DiskFileSystem : IFileSystem
{
    public bool Exists(string path) => File.Exists(path) || Directory.Exists(path);
    public PortFileInfo Stat(string path) =>
        Directory.Exists(path) ? new PortFileInfo(0, true, new Bromelia.Foundation.Instant(0))
        : File.Exists(path) ? new PortFileInfo(new System.IO.FileInfo(path).Length, false, new Bromelia.Foundation.Instant(0))
        : throw new Bromelia.Foundation.BroFailure(new Bromelia.Foundation.BroError("fs.notFound"));
    /// <summary>As PlatformFileSystem: fs.notFound when nothing is there, fs.failed for a file.</summary>
    public IReadOnlyList<DirectoryEntry> List(string directory)
    {
        if (!Directory.Exists(directory))
            throw new Bromelia.Foundation.BroFailure(new Bromelia.Foundation.BroError(File.Exists(directory) ? "fs.failed" : "fs.notFound"));
        return new DirectoryInfo(directory).EnumerateFileSystemInfos().Select(e => new DirectoryEntry(e.Name, e is DirectoryInfo)).ToList();
    }
    public byte[] Read(string path) => File.ReadAllBytes(path);
    public byte[] ReadRange(string path, long offset, int length) => throw new NotSupportedException();
    public void CreateDirectory(string path, bool parentsMustExist) => Directory.CreateDirectory(path);

    public void WriteAtomically(string path, byte[] bytes, int mode)
    {
        var temp = path + ".tmp";
        File.WriteAllBytes(temp, bytes);
        if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(temp, (UnixFileMode)mode);
        File.Move(temp, path, overwrite: true);
    }

    public void Rename(string from, string to) => File.Move(from, to);
    public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy) => throw new NotSupportedException();
    public void Remove(string path) => File.Delete(path);
    public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
    public void SyncFile(string path) { }
    public void SyncDirectory(string path) { }
    public IByteStream OpenForReading(string path, bool bypassCache) => throw new NotSupportedException();
    public VolumeInfo Volume(string path) => throw new NotSupportedException();
}
