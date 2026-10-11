using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>The steps by kind. The composition root registers the standard steps; tests register their own.</summary>
public sealed class StepRegistry
{
    readonly Dictionary<StepKind, IStep> _steps;

    public StepRegistry(IEnumerable<IStep> steps)
    {
        _steps = steps.ToDictionary(s => s.Kind);
    }

    public IStep? Step(StepKind kind) => _steps.GetValueOrDefault(kind);
}
