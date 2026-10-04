using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of commit_items.</summary>
public sealed record CommitItem(
    int Seq,
    string FromPath,
    string ToPath,
    string? Sha256,
    bool IsDir,
    bool Moved);
