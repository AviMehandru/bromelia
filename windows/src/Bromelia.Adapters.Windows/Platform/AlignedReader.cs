using System;
using System.Runtime.InteropServices;

namespace Bromelia.Adapters.Windows;

/// <summary>Reads into <paramref name="buffer"/> from <paramref name="offset"/>; 0 at the end of the file.</summary>
internal delegate int ReadAt(Span<byte> buffer, long offset);

/// <summary>A file read front to back under FILE_FLAG_NO_BUFFERING, where a read must start at a multiple of the sector
/// size, into an aligned buffer, served to the caller from there. A read that gives less than it was asked for isn't the
/// end of the file, a share may do that: the next read starts at the sector it stopped in, and the file ends only when a
/// read adds nothing.</summary>
internal sealed unsafe class AlignedReader : IDisposable
{
    const int Chunk = 1 << 20; // a multiple of every sector size
    const int Sector = 4096; // the largest in use, and a multiple of the others

    readonly ReadAt _read;
    readonly byte* _buffer;
    long _offset;
    int _start, _end;
    bool _eof;

    public AlignedReader(ReadAt read)
    {
        _read = read;
        _buffer = (byte*)NativeMemory.AlignedAlloc(Chunk, Sector);
    }

    public byte[] Read(int maxBytes)
    {
        if (_start == _end && !_eof)
        {
            long from = _offset / Sector * Sector;
            int skip = (int)(_offset - from);
            int n = _read(new Span<byte>(_buffer, Chunk), from);
            _eof = n <= skip;
            _start = _eof ? 0 : skip;
            _end = _eof ? 0 : n;
            _offset += _end - _start;
        }
        int take = Math.Min(maxBytes, _end - _start);
        var result = new ReadOnlySpan<byte>(_buffer + _start, take).ToArray();
        _start += take;
        return result;
    }

    public void Dispose() => NativeMemory.AlignedFree(_buffer);
}
