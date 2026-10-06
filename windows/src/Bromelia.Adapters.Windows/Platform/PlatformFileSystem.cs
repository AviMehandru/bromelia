using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Win32.SafeHandles;
using PortFileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Adapters.Windows;

/// <summary>The FileSystem port on Windows (plan §9, §10.3; shared/fixtures/adapters/file-system.cases.json): writes
/// are flushed with FlushFileBuffers and renamed with MOVEFILE_WRITE_THROUGH, nothing is ever replaced except by
/// writeAtomically, a missing parent is created only when asked, uncached reads use FILE_FLAG_NO_BUFFERING, and the
/// volume comes from GetVolumeInformationW. Windows has no POSIX permission bits, so the mode is ignored.</summary>
public sealed class PlatformFileSystem : IFileSystem
{
    public bool Exists(string path) => File.Exists(path) || Directory.Exists(path);

    public PortFileInfo Stat(string path)
    {
        if (Directory.Exists(path)) return new PortFileInfo(0, true, Time(Directory.GetLastWriteTimeUtc(path)));
        if (File.Exists(path)) return new PortFileInfo(new System.IO.FileInfo(path).Length, false, Time(File.GetLastWriteTimeUtc(path)));
        throw NotFound(path);
    }

    public IReadOnlyList<DirectoryEntry> List(string directory)
    {
        if (!Directory.Exists(directory)) throw File.Exists(directory) ? Failed("list", directory, "it isn't a folder") : NotFound(directory);
        return Guard("list", directory, () => new DirectoryInfo(directory).EnumerateFileSystemInfos()
            .Select(e => new DirectoryEntry(e.Name, (e.Attributes & FileAttributes.Directory) != 0)).ToList());
    }

    public byte[] Read(string path)
    {
        if (!File.Exists(path)) throw Directory.Exists(path) ? Failed("read", path, "it is a folder") : NotFound(path);
        return Guard("read", path, () => File.ReadAllBytes(path));
    }

    public byte[] ReadRange(string path, long offset, int length)
    {
        if (!File.Exists(path)) throw Directory.Exists(path) ? Failed("read", path, "it is a folder") : NotFound(path);
        return Guard("read", path, () =>
        {
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (offset >= f.Length) return Array.Empty<byte>();
            f.Seek(offset, SeekOrigin.Begin);
            var buffer = new byte[(int)Math.Min(length, f.Length - offset)];
            int got = 0, n;
            while (got < buffer.Length && (n = f.Read(buffer, got, buffer.Length - got)) > 0) got += n;
            return got == buffer.Length ? buffer : buffer[..got];
        });
    }

    public void CreateDirectory(string path, bool parentsMustExist)
    {
        if (Directory.Exists(path)) return;
        if (File.Exists(path)) throw AlreadyExists(path);
        var parent = Parent(path);
        if (parentsMustExist && parent != null && !Directory.Exists(parent)) throw ParentMissing(parent);
        Guard("createDirectory", path, () => Directory.CreateDirectory(path));
    }

    public void WriteAtomically(string path, byte[] bytes, int mode)
    {
        var parent = Parent(path) ?? throw Failed("write", path, "no folder");
        if (!Directory.Exists(parent)) throw ParentMissing(parent);
        var temp = Path.Combine(parent, "." + Path.GetFileName(path) + ".bromelia-tmp-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            Guard("write", path, () =>
            {
                using (var f = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                {
                    f.Write(bytes, 0, bytes.Length);
                    f.Flush(flushToDisk: true);
                }
                if (!Native.MoveFileExW(temp, path, Native.MOVEFILE_REPLACE_EXISTING | Native.MOVEFILE_WRITE_THROUGH))
                    throw new IOException(Native.LastError());
            });
            SyncDirectory(parent);
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    public void Rename(string from, string to)
    {
        if (!Exists(from)) throw NotFound(from);
        if (Exists(to)) throw AlreadyExists(to);
        var parent = Parent(to);
        if (parent != null && !Directory.Exists(parent)) throw ParentMissing(parent);
        // No MOVEFILE_REPLACE_EXISTING: Windows refuses to replace, so a file that appears meanwhile is kept.
        if (!Native.MoveFileExW(from, to, Native.MOVEFILE_WRITE_THROUGH))
        {
            if (Exists(to)) throw AlreadyExists(to);
            throw Failed("rename", from, Native.LastError());
        }
    }

    public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null)
    {
        if (!Directory.Exists(from)) throw File.Exists(from) ? Failed("move", from, "it isn't a folder") : NotFound(from);
        if (!Exists(to)) CreateDirectory(to, parentsMustExist: true);
        else if (!Directory.Exists(to)) throw AlreadyExists(to);
        var moved = new List<MovedItem>();
        Merge(from, to, item =>
        {
            moved.Add(item);
            onMoved?.Invoke(item);
        });
        return moved.OrderBy(m => m.From, StringComparer.Ordinal).ToList();
    }

    /// <summary>Each entry of <paramref name="source"/> in turn: a folder merges into a folder of the same name
    /// (ignoring ASCII case); anything else moves under ConflictNamer's name. Emptied source folders go.</summary>
    void Merge(string source, string target, Action<MovedItem> moved)
    {
        foreach (var e in List(source).OrderBy(e => e.Name, StringComparer.Ordinal))
        {
            var src = Path.Combine(source, e.Name);
            var existing = List(target);
            var same = existing.FirstOrDefault(x => SameIgnoringAsciiCase(x.Name, e.Name));
            if (e.IsDirectory && same is { IsDirectory: true })
            {
                Merge(src, Path.Combine(target, same.Name), moved);
                Remove(src);
                continue;
            }
            var dst = Path.Combine(target, ConflictNamer.Next(e.Name, existing.Select(x => x.Name).ToList(), e.IsDirectory));
            Rename(src, dst);
            if (!e.IsDirectory)
                moved(new MovedItem(src, dst));
            else
                foreach (var file in Directory.EnumerateFiles(dst, "*", SearchOption.AllDirectories))
                    moved(new MovedItem(Path.Combine(src, Path.GetRelativePath(dst, file)), file));
        }
    }

    public void Remove(string path)
    {
        if (Directory.Exists(path))
        {
            if (Directory.EnumerateFileSystemEntries(path).Any()) throw Failed("remove", path, "the folder isn't empty");
            Guard("remove", path, () => Directory.Delete(path, recursive: false));
        }
        else if (File.Exists(path))
            Guard("remove", path, () => File.Delete(path));
        else
            throw NotFound(path);
    }

    public void MoveToTrash(string path, string trash)
    {
        if (!Exists(path)) throw NotFound(path);
        CreateDirectory(trash, parentsMustExist: true);
        var name = ConflictNamer.Next(Path.GetFileName(path.TrimEnd('\\', '/')), List(trash).Select(e => e.Name).ToList(), Directory.Exists(path));
        Rename(path, Path.Combine(trash, name));
    }

    public void SyncFile(string path)
    {
        if (!File.Exists(path)) throw Directory.Exists(path) ? Failed("sync", path, "it is a folder") : NotFound(path);
        Guard("sync", path, () =>
        {
            using var f = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
            f.Flush(flushToDisk: true);
        });
    }

    /// <summary>FlushFileBuffers on the folder (opened with FILE_FLAG_BACKUP_SEMANTICS): the rename that just
    /// happened in it reaches the disk. A file system that can't flush a folder at all (the SMB client answers
    /// ERROR_INVALID_FUNCTION) has nothing to do: the rename is already the server's to keep.</summary>
    public void SyncDirectory(string path)
    {
        if (!Directory.Exists(path)) throw NotFound(path);
        using var h = Native.CreateFileW(path, Native.GENERIC_READ | Native.GENERIC_WRITE, FileShare.ReadWrite | FileShare.Delete, IntPtr.Zero,
            FileMode.Open, Native.FILE_FLAG_BACKUP_SEMANTICS, IntPtr.Zero);
        if (h.IsInvalid) throw Failed("sync", path, Native.LastError());
        if (Native.FlushFileBuffers(h)) return;
        var code = Marshal.GetLastWin32Error();
        if (code is Native.ERROR_INVALID_FUNCTION or Native.ERROR_NOT_SUPPORTED) return;
        throw Failed("sync", path, new System.ComponentModel.Win32Exception(code).Message);
    }

    public IByteStream OpenForReading(string path, bool bypassCache)
    {
        if (!File.Exists(path)) throw Directory.Exists(path) ? Failed("read", path, "it is a folder") : NotFound(path);
        return Guard("read", path, () => bypassCache ? (IByteStream)new UncachedStream(path) : new CachedStream(path));
    }

    /// <summary>The volume holding <paramref name="path"/>, or its nearest existing parent: the volume serial number,
    /// the bytes free for this user, the file system's name. Windows compares names without case.</summary>
    public VolumeInfo Volume(string path)
    {
        var existing = Path.GetFullPath(path);
        while (!Exists(existing) && Parent(existing) is { } up) existing = up;
        var root = new StringBuilder(1024);
        if (!Native.GetVolumePathNameW(existing, root, (uint)root.Capacity)) throw Failed("volume", path, Native.LastError());
        var fs = new StringBuilder(261);
        if (!Native.GetVolumeInformationW(root.ToString(), null, 0, out var serial, out _, out _, fs, (uint)fs.Capacity))
            throw Failed("volume", path, Native.LastError());
        if (!Native.GetDiskFreeSpaceExW(existing, out var free, out _, out _)) throw Failed("volume", path, Native.LastError());
        return new VolumeInfo(serial.ToString("X8"), (long)free, fs.ToString(), false);
    }

    // ---- helpers ---------------------------------------------------------------------------------

    static bool SameIgnoringAsciiCase(string a, string b) =>
        a.Length == b.Length && a.Zip(b).All(p => (p.First is >= 'A' and <= 'Z' ? (char)(p.First + 32) : p.First) == (p.Second is >= 'A' and <= 'Z' ? (char)(p.Second + 32) : p.Second));

    static Instant Time(DateTime utc) => new(new DateTimeOffset(DateTime.SpecifyKind(utc, DateTimeKind.Utc)).ToUnixTimeMilliseconds());

    static string? Parent(string path) => Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)));

    static BroFailure Fail(MessageCode code, params (string Key, JsonValue Value)[] p) => new(new BroMessage(code, Severity.Error, p).ToError());
    static BroFailure NotFound(string path) => Fail(MessageCode.FsNotFound, ("path", JsonValue.Of(path)));
    static BroFailure AlreadyExists(string path) => Fail(MessageCode.FsAlreadyExists, ("path", JsonValue.Of(path)));
    static BroFailure ParentMissing(string path) => Fail(MessageCode.FsParentMissing, ("path", JsonValue.Of(path)));
    static BroFailure Failed(string operation, string path, string reason) =>
        Fail(MessageCode.FsFailed, ("operation", JsonValue.Of(operation)), ("path", JsonValue.Of(path)), ("reason", JsonValue.Of(reason)));

    static T Guard<T>(string operation, string path, Func<T> body)
    {
        try { return body(); }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { throw NotFound(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { throw Failed(operation, path, e.Message); }
    }

    static void Guard(string operation, string path, Action body) => Guard(operation, path, () => { body(); return 0; });

    sealed class CachedStream : IByteStream
    {
        readonly FileStream _f;
        public CachedStream(string path) { _f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan); }

        public byte[] Read(int maxBytes)
        {
            var buffer = new byte[maxBytes];
            int n = _f.Read(buffer, 0, maxBytes);
            return n == maxBytes ? buffer : buffer[..n];
        }

        public void Close() => _f.Dispose();
    }

    /// <summary>FILE_FLAG_NO_BUFFERING: every read is a multiple of the sector size into an aligned buffer, served
    /// to the caller from there.</summary>
    sealed unsafe class UncachedStream : IByteStream
    {
        const int Chunk = 1 << 20; // a multiple of every sector size
        readonly SafeFileHandle _h;
        readonly byte* _buffer;
        long _offset;
        int _start, _end;
        bool _eof;

        public UncachedStream(string path)
        {
            _h = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read, (FileOptions)Native.FILE_FLAG_NO_BUFFERING);
            _buffer = (byte*)NativeMemory.AlignedAlloc(Chunk, 4096);
        }

        public byte[] Read(int maxBytes)
        {
            if (_start == _end && !_eof)
            {
                int n = RandomAccess.Read(_h, new Span<byte>(_buffer, Chunk), _offset);
                _offset += n;
                _start = 0;
                _end = n;
                _eof = n < Chunk;
            }
            int take = Math.Min(maxBytes, _end - _start);
            var result = new ReadOnlySpan<byte>(_buffer + _start, take).ToArray();
            _start += take;
            return result;
        }

        public void Close()
        {
            _h.Dispose();
            NativeMemory.AlignedFree(_buffer);
        }
    }

    static class Native
    {
        public const uint MOVEFILE_REPLACE_EXISTING = 0x1;
        public const uint MOVEFILE_WRITE_THROUGH = 0x8;
        public const uint GENERIC_READ = 0x80000000;
        public const uint GENERIC_WRITE = 0x40000000;
        public const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
        public const int FILE_FLAG_NO_BUFFERING = 0x20000000;
        public const int ERROR_INVALID_FUNCTION = 1;
        public const int ERROR_NOT_SUPPORTED = 50;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool MoveFileExW(string from, string to, uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafeFileHandle CreateFileW(string name, uint access, FileShare share, IntPtr security, FileMode mode, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool FlushFileBuffers(SafeFileHandle handle);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool GetVolumePathNameW(string path, StringBuilder root, uint length);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool GetVolumeInformationW(string root, StringBuilder? name, uint nameLength, out uint serial, out uint maxComponent,
            out uint flags, StringBuilder fsName, uint fsNameLength);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool GetDiskFreeSpaceExW(string path, out ulong freeForCaller, out ulong total, out ulong free);

        public static string LastError() => new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message;
    }
}
