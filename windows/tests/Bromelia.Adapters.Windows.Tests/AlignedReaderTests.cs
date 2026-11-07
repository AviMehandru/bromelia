using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Bromelia.Adapters.Windows;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>FILE_FLAG_NO_BUFFERING's reader: a short read isn't the end of the file, and every read starts on a sector.</summary>
public sealed class AlignedReaderTests
{
    const int Chunk = 1 << 20;

    static byte[] Pattern(int size) => Enumerable.Range(0, size).Select(i => (byte)(i * 31 + (i >> 8))).ToArray();

    /// <summary>All of a file, read through a source that gives at most <paramref name="most"/> bytes a read.</summary>
    static (byte[] Bytes, List<long> Offsets) ReadAll(byte[] file, int most)
    {
        var offsets = new List<long>();
        using var reader = new AlignedReader((buffer, offset) =>
        {
            offsets.Add(offset);
            int n = (int)Math.Max(0, Math.Min(Math.Min(most, buffer.Length), file.Length - offset));
            if (n > 0) file.AsSpan((int)offset, n).CopyTo(buffer);
            return n;
        });
        var read = new List<byte>();
        for (var piece = reader.Read(Chunk); piece.Length > 0; piece = reader.Read(Chunk)) read.AddRange(piece);
        return (read.ToArray(), offsets);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(4095)]
    [InlineData(4096)]
    [InlineData(4097)]
    [InlineData(Chunk - 1)]
    [InlineData(Chunk)]
    [InlineData(Chunk + 1)]
    [InlineData(3 * Chunk + 5000)]
    public void AFileOfAnySizeComesBackWhole(int size)
    {
        var file = Pattern(size);
        var (bytes, offsets) = ReadAll(file, Chunk);
        Assert.Equal(file, bytes);
        Assert.All(offsets, o => Assert.Equal(0, o % 4096));
    }

    [Theory]
    [InlineData(4096)]
    [InlineData(5000)] // stops inside a sector
    [InlineData(65537)]
    [InlineData(Chunk - 512)]
    public void AShortReadIsNotTheEndOfTheFile(int most)
    {
        var file = Pattern(2 * Chunk + 777);
        var (bytes, offsets) = ReadAll(file, most);
        Assert.Equal(file, bytes);
        Assert.All(offsets, o => Assert.Equal(0, o % 4096));
    }

    [Theory]
    [InlineData(5000)]
    [InlineData(4096)]
    [InlineData(Chunk)]
    [InlineData(Chunk + 1)]
    public void ARealFileComesBackWholeWithoutTheCache(int size)
    {
        var folder = Path.Combine(Path.GetTempPath(), "bro-aligned-" + Guid.NewGuid());
        Directory.CreateDirectory(folder);
        try
        {
            var path = Path.Combine(folder, "file");
            var file = Pattern(size);
            File.WriteAllBytes(path, file);
            var stream = new PlatformFileSystem().OpenForReading(path, bypassCache: true);
            var read = new List<byte>();
            for (var piece = stream.Read(Chunk); piece.Length > 0; piece = stream.Read(Chunk)) read.AddRange(piece);
            stream.Close();
            Assert.Equal(file, read.ToArray());
        }
        finally
        {
            Directory.Delete(folder, recursive: true);
        }
    }
}
