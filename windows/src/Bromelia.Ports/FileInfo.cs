using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A file's size, whether it is a folder, and when it last changed.</summary>
public sealed record FileInfo(
    long Size,
    bool IsDirectory,
    Instant ModifiedAt);
