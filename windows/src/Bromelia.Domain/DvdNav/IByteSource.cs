using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>The one input interface defined in Domain: the files of a VIDEO_TS folder, wherever they are (an ISO, a
/// folder or a disc). Adapters implement it over FileSystem or SectorReader.</summary>
public interface IByteSource
{
    /// <summary><paramref name="length"/> bytes of <paramref name="path"/> (an upper-case file name such as
    /// <c>VTS_01_0.IFO</c>) from byte <paramref name="offset"/>; fewer at the end of the file, none when it can't be
    /// read.</summary>
    byte[] Read(string path, long offset, int length);

    /// <summary>Every file, by name.</summary>
    IReadOnlyList<ByteFile> Files();
}
