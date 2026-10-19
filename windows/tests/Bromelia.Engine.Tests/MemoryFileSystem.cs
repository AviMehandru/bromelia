using Bromelia.Foundation;
using Bromelia.Ports;
using FileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Engine.Tests;

/// <summary>Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe. While
/// <see cref="Recording"/> is on, the calls that change something are written to <see cref="Ops"/> ('write', 'sync',
/// 'remove', 'rename'); <see cref="Put"/> and <see cref="PutFolder"/> (a tool writing, a case's setup) aren't.</summary>
internal sealed class MemoryFileSystem : IFileSystem
{
    readonly object _lock = new();
    readonly Dictionary<string, byte[]> _files = new();
    readonly HashSet<string> _folders = new() { "/" };
    /// <summary>Reads of these paths fail so.</summary>
    public readonly Dictionary<string, BroError> ReadFails = new();
    /// <summary>What <see cref="Volume"/> gives for these paths.</summary>
    public readonly Dictionary<string, VolumeInfo> Volumes = new();
    public readonly List<string> Ops = new();
    public bool Recording;

    static BroFailure NotFound(string path) => new(new BroError("fs.notFound", ("path", JsonValue.Of(path))));

    static string Parent(string path) => path.LastIndexOf('/') is var i && i > 0 ? path[..i] : "/";

    void Record(string op)
    {
        if (Recording) Ops.Add(op);
    }

    /// <summary>A file with these bytes, its folders made.</summary>
    public void Put(string path, byte[] bytes)
    {
        lock (_lock)
        {
            PutFolderLocked(Parent(path));
            _files[path] = bytes.ToArray();
        }
    }

    public void PutFolder(string path)
    {
        lock (_lock) PutFolderLocked(path);
    }

    void PutFolderLocked(string path)
    {
        var current = "";
        foreach (var p in path.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            current += "/" + p;
            _folders.Add(current);
        }
    }

    public bool Exists(string path)
    {
        lock (_lock) return _files.ContainsKey(path) || _folders.Contains(path);
    }

    public byte[] Read(string path)
    {
        lock (_lock)
            return ReadFails.TryGetValue(path, out var fails) ? throw new BroFailure(fails)
                : _files.TryGetValue(path, out var b) ? b
                : throw NotFound(path);
    }

    public void CreateDirectory(string path, bool parentsMustExist) => PutFolder(path);

    public void WriteAtomically(string path, byte[] bytes, int mode)
    {
        lock (_lock)
        {
            var folder = path[..path.LastIndexOf('/')];
            if (!_folders.Contains(folder)) throw NotFound(folder);
            _files[path] = bytes.ToArray();
            Record("write " + path);
        }
    }

    public FileInfo Stat(string path)
    {
        lock (_lock)
            return _files.TryGetValue(path, out var b) ? new FileInfo(b.Length, false, new Instant(0))
                : _folders.Contains(path) ? new FileInfo(0, true, new Instant(0))
                : throw NotFound(path);
    }

    public IReadOnlyList<DirectoryEntry> List(string directory)
    {
        lock (_lock)
        {
            if (!_folders.Contains(directory)) throw NotFound(directory);
            var prefix = directory == "/" ? "/" : directory + "/";
            bool Child(string p) => p.StartsWith(prefix, StringComparison.Ordinal) && p.Length > prefix.Length && p.IndexOf('/', prefix.Length) < 0;
            return _files.Keys.Where(Child).Select(p => new DirectoryEntry(p[prefix.Length..], false))
                .Concat(_folders.Where(Child).Select(p => new DirectoryEntry(p[prefix.Length..], true)))
                .OrderBy(e => e.Name, StringComparer.Ordinal).ToList();
        }
    }

    public byte[] ReadRange(string path, long offset, int length)
    {
        var bytes = Read(path);
        if (offset >= bytes.Length) return Array.Empty<byte>();
        return bytes.Skip((int)offset).Take(length).ToArray();
    }

    public void Rename(string from, string to)
    {
        lock (_lock)
        {
            if (Exists(to)) throw new BroFailure(new BroError("fs.alreadyExists", ("path", JsonValue.Of(to))));
            if (_files.Remove(from, out var b)) _files[to] = b;
            else if (_folders.Contains(from))
            {
                foreach (var f in _files.Keys.Where(p => p.StartsWith(from + "/", StringComparison.Ordinal)).ToList())
                {
                    _files[to + f[from.Length..]] = _files[f];
                    _files.Remove(f);
                }
                foreach (var d in _folders.Where(p => p == from || p.StartsWith(from + "/", StringComparison.Ordinal)).ToList())
                {
                    _folders.Remove(d);
                    _folders.Add(to + d[from.Length..]);
                }
            }
            else throw NotFound(from);
            Record($"rename {from} {to}");
        }
    }

    public void Remove(string path)
    {
        lock (_lock)
        {
            if (_files.Remove(path))
            {
                Record("remove " + path);
                return;
            }
            if (!_folders.Contains(path)) throw NotFound(path);
            if (_files.Keys.Concat(_folders).Any(p => p.StartsWith(path + "/", StringComparison.Ordinal)))
                throw new BroFailure(new BroError("fs.failed", ("path", JsonValue.Of(path)), ("reason", JsonValue.Of("not empty"))));
            _folders.Remove(path);
            Record("remove " + path);
        }
    }

    public void SyncFile(string path)
    {
        lock (_lock)
        {
            if (!_files.ContainsKey(path)) throw NotFound(path);
            Record("sync " + path);
        }
    }

    public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null) => throw new NotSupportedException();
    public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
    public void SyncDirectory(string path)
    {
        lock (_lock)
        {
            if (!_folders.Contains(path)) throw NotFound(path);
            Record("syncDirectory " + path);
        }
    }
    public IByteStream OpenForReading(string path, bool bypassCache) => throw new NotSupportedException();

    public VolumeInfo Volume(string path)
    {
        lock (_lock)
            return Volumes.TryGetValue(path, out var v) ? v : throw NotFound(path);
    }
}
