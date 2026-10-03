using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>What analyse found: every title's chapters, how the menus and pre-commands reach chapters, and the
/// menu stills.</summary>
public sealed record NavAnalysis(IReadOnlyList<NavTitle> Titles, IReadOnlyList<NavJump> Jumps, IReadOnlyList<CellRef> Stills);
