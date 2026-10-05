using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Files and folders. Paths are absolute.</summary>
public interface IFileSystem
{
    /// <summary>Whether anything is at path.</summary>
    bool Exists(string path);

    FileInfo Stat(string path);

    /// <summary>The entries of a folder, in no particular order.</summary>
    IReadOnlyList<DirectoryEntry> List(string directory);

    byte[] Read(string path);

    /// <summary>Fewer bytes at the end of the file.</summary>
    byte[] ReadRange(string path, long offset, int length);

    /// <summary>Creates a folder (and, unless parentsMustExist, its parents). Never creates a library root (plan
    /// §22).</summary>
    void CreateDirectory(string path, bool parentsMustExist);

    /// <summary>A temporary file, fsync, rename over path, fsync of the folder; mode is the POSIX permission
    /// bits. A folder sync that fails fails the call although the new content is in place: a retry replaces it, so
    /// it is safe.</summary>
    void WriteAtomically(string path, byte[] bytes, int mode);

    void Rename(string from, string to);

    /// <summary>Moves a folder's contents into another, merging folders. <paramref name="onMoved"/> hears of each item
    /// as soon as it has moved, so a failure part-way (thrown) still says what moved.</summary>
    IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null);

    /// <summary>Removes a file or an empty folder.</summary>
    void Remove(string path);

    /// <summary>Moves path into the trash folder (never deletes).</summary>
    void MoveToTrash(string path, string trash);

    /// <summary>F_FULLFSYNC / FlushFileBuffers / fsync.</summary>
    void SyncFile(string path);

    void SyncDirectory(string path);

    /// <summary>F_NOCACHE / NO_BUFFERING / fadvise when bypassCache.</summary>
    IByteStream OpenForReading(string path, bool bypassCache);

    VolumeInfo Volume(string path);
}
