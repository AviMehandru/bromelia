using Bromelia.Foundation;
using Bromelia.Ports;
using FileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Engine.Tests;

/// <summary>Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe.</summary>
internal sealed class MemoryFileSystem : IFileSystem
{
    readonly object _lock = new();
    readonly Dictionary<string, byte[]> _files = new();
    readonly HashSet<string> _folders = new() { "/" };
    /// <summary>Reads of these paths fail so.</summary>
    public readonly Dictionary<string, BroError> ReadFails = new();

    public bool Exists(string path)
    {
        lock (_lock) return _files.ContainsKey(path) || _folders.Contains(path);
    }

    public byte[] Read(string path)
    {
        lock (_lock)
            return ReadFails.TryGetValue(path, out var fails) ? throw new BroFailure(fails)
                : _files.TryGetValue(path, out var b) ? b
                : throw new BroFailure(new BroError("fs.notFound", ("path", JsonValue.Of(path))));
    }

    public void CreateDirectory(string path, bool parentsMustExist)
    {
        lock (_lock)
        {
            var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
            var current = "";
            foreach (var p in parts)
            {
                current += "/" + p;
                _folders.Add(current);
            }
        }
    }

    public void WriteAtomically(string path, byte[] bytes, int mode)
    {
        lock (_lock)
        {
            var folder = path[..path.LastIndexOf('/')];
            if (!_folders.Contains(folder)) throw new BroFailure(new BroError("fs.notFound", ("path", JsonValue.Of(folder))));
            _files[path] = bytes.ToArray();
        }
    }

    public FileInfo Stat(string path) => throw new NotSupportedException();
    public IReadOnlyList<DirectoryEntry> List(string directory) => throw new NotSupportedException();
    public byte[] ReadRange(string path, long offset, int length) => throw new NotSupportedException();
    public void Rename(string from, string to) => throw new NotSupportedException();
    public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null) => throw new NotSupportedException();
    public void Remove(string path) => throw new NotSupportedException();
    public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
    public void SyncFile(string path) => throw new NotSupportedException();
    public void SyncDirectory(string path) => throw new NotSupportedException();
    public IByteStream OpenForReading(string path, bool bypassCache) => throw new NotSupportedException();
    public VolumeInfo Volume(string path) => throw new NotSupportedException();
}
