using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A disc title: its number, chapter lengths in seconds and the VOB id of each chapter.</summary>
public sealed record NavTitle(int Number, IReadOnlyList<double> Chapters, IReadOnlyList<int> ChapterVobs);
