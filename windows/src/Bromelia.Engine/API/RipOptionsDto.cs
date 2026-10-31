using System.Collections.Generic;
using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>How a client asks for a rip (dtos.json RipOptions): the mode, profile and library to use instead of what the
/// rules give, the titles of the open session, and a one-off output folder that must be a library's.</summary>
public sealed record RipOptionsDto(RipMode? Mode = null, string? Profile = null, string? Library = null, IReadOnlyList<int>? Titles = null,
    string? OutputFolder = null);
