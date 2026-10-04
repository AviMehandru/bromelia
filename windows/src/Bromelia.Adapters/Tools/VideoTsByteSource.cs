using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>The Domain ByteSource over a DVD (shared/fixtures/adapters/video-ts-byte-source.cases.json): VIDEO_TS inside
/// an ISO 9660 / UDF bridge image (or a disc device, read the same way), a VIDEO_TS folder, or the folder that contains
/// one. Reads stop at the end of a file.</summary>
public sealed class VideoTsByteSource : IByteSource, IDisposable
{
    const int Sector = 2048;

    readonly FileStream? _image;                    // an image: its files' first sectors
    readonly Dictionary<string, long> _lba = new();
    readonly Dictionary<string, string> _paths = new(); // a folder: its files' paths
    readonly Dictionary<string, long> _sizes = new();
    readonly string _label;
    readonly object _gate = new();

    VideoTsByteSource(FileStream? image, string label)
    {
        _image = image;
        _label = label;
    }

    /// <summary>None when <paramref name="path"/> is neither an image with VIDEO_TS nor a folder with (or that is)
    /// VIDEO_TS holding VIDEO_TS.IFO.</summary>
    public static VideoTsByteSource? Open(string path)
    {
        try
        {
            if (Directory.Exists(path)) return OpenFolder(path);
            return File.Exists(path) || path.StartsWith(@"\\.\", StringComparison.Ordinal) ? OpenImage(path) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>The image's volume name, or the name of the folder that contains VIDEO_TS.</summary>
    public string Label() => _label;

    public IReadOnlyList<ByteFile> Files() =>
        _sizes.Select(f => new ByteFile(f.Key, f.Value)).OrderBy(f => f.Name, StringComparer.Ordinal).ToList();

    public byte[] Read(string path, long offset, int length)
    {
        if (!_sizes.TryGetValue(path, out var size) || offset < 0 || offset >= size || length <= 0) return Array.Empty<byte>();
        var count = (int)Math.Min(length, size - offset);
        try
        {
            if (_image is null)
            {
                using var file = new FileStream(_paths[path], FileMode.Open, FileAccess.Read, FileShare.Read);
                return ReadAt(file, offset, count);
            }
            lock (_gate) return ReadSectors(_image, _lba[path] * Sector + offset, count);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return Array.Empty<byte>(); }
    }

    public void Dispose() => _image?.Dispose();

    static VideoTsByteSource? OpenFolder(string path)
    {
        var dir = path.TrimEnd('/', '\\');
        if (!string.Equals(Path.GetFileName(dir), "VIDEO_TS", StringComparison.OrdinalIgnoreCase))
        {
            var sub = Directory.EnumerateDirectories(dir).FirstOrDefault(d => string.Equals(Path.GetFileName(d), "VIDEO_TS", StringComparison.OrdinalIgnoreCase));
            if (sub is null) return null;
            dir = sub;
        }
        var source = new VideoTsByteSource(null, Path.GetFileName(Path.GetDirectoryName(dir)) ?? "");
        foreach (var f in Directory.EnumerateFiles(dir))
        {
            var name = Path.GetFileName(f).ToUpperInvariant();
            source._paths[name] = f;
            source._sizes[name] = new FileInfo(f).Length;
        }
        return source._sizes.ContainsKey("VIDEO_TS.IFO") ? source : null;
    }

    static VideoTsByteSource? OpenImage(string path)
    {
        var image = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            var pvd = ReadSectors(image, 16L * Sector, Sector);
            if (pvd.Length < 190 || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001") return Fail(image);
            var source = new VideoTsByteSource(image, Encoding.ASCII.GetString(pvd, 40, 32).Trim(' ', '\0'));
            var root = pvd.AsSpan(156, 34).ToArray();
            var videoTs = Directory_(image, U32(root, 2), U32(root, 10)).FirstOrDefault(e => e.Name.ToUpperInvariant() == "VIDEO_TS");
            if (videoTs.Name is null) return Fail(image);
            foreach (var e in Directory_(image, videoTs.Lba, videoTs.Size))
            {
                source._lba[e.Name.ToUpperInvariant()] = e.Lba;
                source._sizes[e.Name.ToUpperInvariant()] = e.Size;
            }
            return source;
        }
        catch
        {
            image.Dispose();
            throw;
        }
    }

    static VideoTsByteSource? Fail(FileStream image)
    {
        image.Dispose();
        return null;
    }

    /// <summary>The entries of an ISO 9660 directory: name (without ";1"), first sector, size.</summary>
    static List<(string Name, long Lba, long Size)> Directory_(FileStream image, long lba, long size)
    {
        var data = ReadSectors(image, lba * Sector, (int)Math.Min(size, 1 << 24));
        var entries = new List<(string, long, long)>();
        int i = 0;
        while (i < data.Length)
        {
            int len = data[i];
            if (len == 0)
            {
                i = (i / Sector + 1) * Sector; // records don't cross sectors
                continue;
            }
            if (i + len > data.Length || len < 34) break;
            int nameLen = data[i + 32];
            if (33 + nameLen <= len)
            {
                var name = Encoding.ASCII.GetString(data, i + 33, nameLen).Split(';')[0];
                if (name != "\0" && name != "\u0001") entries.Add((name, U32(data, i + 2), U32(data, i + 10)));
            }
            i += len;
        }
        return entries;
    }

    static byte[] ReadAt(FileStream file, long offset, int count)
    {
        file.Seek(offset, SeekOrigin.Begin);
        var buffer = new byte[count];
        int total = 0, n;
        while (total < count && (n = file.Read(buffer, total, count - total)) > 0) total += n;
        return total == count ? buffer : buffer[..total];
    }

    /// <summary>Whole sectors around the range, then the range: a disc device only reads whole sectors.</summary>
    static byte[] ReadSectors(FileStream image, long offset, int count)
    {
        var start = offset / Sector * Sector;
        var end = (offset + count + Sector - 1) / Sector * Sector;
        var data = ReadAt(image, start, (int)(end - start));
        var skip = (int)(offset - start);
        return data.Length <= skip ? Array.Empty<byte>() : data[skip..Math.Min(data.Length, skip + count)];
    }

    static long U32(byte[] b, int i) => i + 4 <= b.Length ? b[i] | (long)b[i + 1] << 8 | (long)b[i + 2] << 16 | (long)b[i + 3] << 24 : 0;
}
