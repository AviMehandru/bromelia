using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The volume holding a path: a stable id, the bytes free, the file system and whether names are
/// case-sensitive.</summary>
public sealed record VolumeInfo(
    string Id,
    long FreeBytes,
    string FsType,
    bool CaseSensitive);
