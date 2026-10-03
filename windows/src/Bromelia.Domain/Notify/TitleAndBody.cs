using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A notification's title and the lines of its body.</summary>
public sealed record TitleAndBody(BroMessage Title, IReadOnlyList<BroMessage> Lines);
