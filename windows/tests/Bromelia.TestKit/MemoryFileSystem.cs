using Bromelia.Foundation;
using Bromelia.Ports;
using FileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.TestKit;

/// <summary>Files and folders in memory: what the engine's tests need of the FileSystem port. Thread-safe. While
/// <see cref="Recording"/> is on, the calls that change something are written to <see cref="Ops"/> ('write', 'sync',
/// 'remove', 'rename', 'syncDirectory', and 'open &lt;path&gt; disk|cache'); <see cref="Put"/> and <see cref="PutFolder"/> (a tool
/// writing, a case's setup) aren't. <see cref="Cached"/> is what a read through the cache gives instead of what is on disk;
/// <see cref="Fails"/> makes an operation on a path fail before it acts (open, read, sync, syncDirectory, write, remove,
/// rename: its from path); a rename refuses a path that exists and a folder that isn't there, as the real ones do;
/// <see cref="OnOp"/> hears of each operation before it acts. While <see cref="Freeze"/> holds, every call that changes something
/// fails (fs.failed, reason "frozen"): a killed engine's writes never land.</summary>
public sealed class MemoryFileSystem : IFileSystem
{
    readonly object _lock = new();
    readonly Dictionary<string, byte[]> _files = new();
    readonly HashSet<string> _folders = new() { "/" };
    /// <summary>Reads of these paths fail so.</summary>
    public readonly Dictionary<string, BroError> ReadFails = new();
    /// <summary>What <see cref="Volume"/> gives for these paths.</summary>
    public readonly Dictionary<string, VolumeInfo> Volumes = new();
    /// <summary>What <see cref="Volume"/> gives for a path <see cref="Volumes"/> doesn't have (null: fs.notFound).</summary>
    public VolumeInfo? AnyVolume;
    public readonly List<string> Ops = new();
    public bool Recording;
    /// <summary>What a read that doesn't bypass the cache gives for these paths.</summary>
    public readonly Dictionary<string, byte[]> Cached = new();
    public readonly List<(string Op, string Path, BroError Error)> Fails = new();
    public Action<string, string>? OnOp;
    /// <summary>Hears of each whole-file <see cref="Read"/> before it reads (a read that is slow, say).</summary>
    public Action<string>? OnRead;
    /// <summary>How many pieces of each file were read while <see cref="Recording"/> was on.</summary>
    public readonly Dictionary<string, int> Reads = new();
    /// <summary>The streams OpenForReading gave that aren't closed yet.</summary>
    public int OpenStreams;

    volatile bool _frozen;

    /// <summary>From now on every call that changes something (write, rename, remove, createDirectory, sync, syncDirectory)
    /// fails, until <see cref="Thaw"/>. <see cref="Put"/> and <see cref="PutFolder"/> still work.</summary>
    public void Freeze() => _frozen = true;

    public void Thaw() => _frozen = false;

    public bool Frozen => _frozen;

    void CheckNotFrozen(string op, string path)
    {
        if (_frozen)
            throw new BroFailure(new BroError("fs.failed", ("operation", JsonValue.Of(op)), ("path", JsonValue.Of(path)), ("reason", JsonValue.Of("frozen"))));
    }

    void Check(string op, string path)
    {
        if (op is "write" or "rename" or "remove" or "sync" or "syncDirectory") CheckNotFrozen(op, path);
        OnOp?.Invoke(op, path);
        foreach (var f in Fails)
            if (f.Op == op && f.Path == path) throw new BroFailure(f.Error);
    }

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
        OnRead?.Invoke(path);
        lock (_lock)
            return ReadFails.TryGetValue(path, out var fails) ? throw new BroFailure(fails)
                : _files.TryGetValue(path, out var b) ? b
                : throw NotFound(path);
    }

    public void CreateDirectory(string path, bool parentsMustExist)
    {
        CheckNotFrozen("createDirectory", path);
        PutFolder(path);
    }

    public void WriteAtomically(string path, byte[] bytes, int mode)
    {
        Check("write", path);
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
        Check("rename", from);
        lock (_lock)
        {
            if (Exists(to)) throw new BroFailure(new BroError("fs.alreadyExists", ("path", JsonValue.Of(to))));
            if (!_folders.Contains(Parent(to))) throw new BroFailure(new BroError("fs.parentMissing", ("path", JsonValue.Of(Parent(to)))));
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
        Check("remove", path);
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
        Check("sync", path);
        lock (_lock)
        {
            if (!_files.ContainsKey(path)) throw NotFound(path);
            Record("sync " + path);
        }
    }

    /// <summary>As the platform file systems do it: each entry of <paramref name="from"/> in turn, a folder merging into a
    /// folder of the same name (ASCII case ignored), anything else moving under ConflictNamer's name; emptied folders go.</summary>
    public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null)
    {
        if (!Exists(from)) throw NotFound(from);
        if (!Stat(from).IsDirectory)
            throw new BroFailure(new BroError("fs.failed", ("operation", JsonValue.Of("move")), ("path", JsonValue.Of(from)), ("reason", JsonValue.Of("it isn't a folder"))));
        if (!Exists(to)) CreateDirectory(to, parentsMustExist: true);
        else if (!Stat(to).IsDirectory) throw new BroFailure(new BroError("fs.alreadyExists", ("path", JsonValue.Of(to))));
        var moved = new List<MovedItem>();
        Merge(from, to, m =>
        {
            moved.Add(m);
            onMoved?.Invoke(m);
        });
        return moved.OrderBy(m => m.From, StringComparer.Ordinal).ToList();
    }

    void Merge(string source, string target, Action<MovedItem> moved)
    {
        foreach (var e in List(source).OrderBy(e => e.Name, StringComparer.Ordinal))
        {
            var src = source + "/" + e.Name;
            var existing = List(target);
            var same = existing.FirstOrDefault(x => AsciiLower(x.Name) == AsciiLower(e.Name));
            if (e.IsDirectory && same is { IsDirectory: true })
            {
                Merge(src, target + "/" + same.Name, moved);
                Remove(src);
                continue;
            }
            var dst = target + "/" + Bromelia.Domain.ConflictNamer.Next(e.Name, existing.Select(x => x.Name).ToList(), e.IsDirectory);
            Rename(src, dst);
            if (!e.IsDirectory) moved(new MovedItem(src, dst));
            else
                lock (_lock)
                    foreach (var f in _files.Keys.Where(k => k.StartsWith(dst + "/", StringComparison.Ordinal)).OrderBy(k => k, StringComparer.Ordinal).ToList())
                        moved(new MovedItem(src + f[dst.Length..], f));
        }
    }

    static string AsciiLower(string s) => new(s.Select(c => c is >= 'A' and <= 'Z' ? (char)(c + 32) : c).ToArray());

    public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
    public void SyncDirectory(string path)
    {
        Check("syncDirectory", path);
        lock (_lock)
        {
            if (!_folders.Contains(path)) throw NotFound(path);
            Record("syncDirectory " + path);
        }
    }
    public IByteStream OpenForReading(string path, bool bypassCache)
    {
        Check("open", path);
        lock (_lock)
        {
            if (!_files.TryGetValue(path, out var disk)) throw NotFound(path);
            Record($"open {path} {(bypassCache ? "disk" : "cache")}");
            OpenStreams++;
            return new Stream(this, path, !bypassCache && Cached.TryGetValue(path, out var cached) ? cached : disk);
        }
    }

    /// <summary>A file read a few bytes at a time ('read' fails are checked on each read).</summary>
    sealed class Stream : IByteStream
    {
        readonly MemoryFileSystem _fs;
        readonly string _path;
        readonly byte[] _bytes;
        int _at;
        bool _closed;

        public Stream(MemoryFileSystem fs, string path, byte[] bytes)
        {
            _fs = fs;
            _path = path;
            _bytes = bytes;
        }

        public byte[] Read(int maxBytes)
        {
            _fs.Check("read", _path);
            lock (_fs._lock)
                if (_fs.Recording) _fs.Reads[_path] = _fs.Reads.GetValueOrDefault(_path) + 1;
            var n = Math.Min(Math.Min(maxBytes, 16), _bytes.Length - _at);
            var piece = _bytes.Skip(_at).Take(n).ToArray();
            _at += n;
            return piece;
        }

        public void Close()
        {
            lock (_fs._lock)
                if (!_closed)
                {
                    _closed = true;
                    _fs.OpenStreams--;
                }
        }
    }

    public VolumeInfo Volume(string path)
    {
        lock (_lock)
            return Volumes.TryGetValue(path, out var v) ? v : AnyVolume ?? throw NotFound(path);
    }
}
