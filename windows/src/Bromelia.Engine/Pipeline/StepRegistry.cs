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

    /// <summary>The phase 3 steps; both composition roots use it, so their lists can't drift.</summary>
    public static StepRegistry Standard() => new(new IStep[]
    {
        new AwaitMediaStep(), new ProbeStep(), new ReconcileStep(), new IdentifyStep(), new PlanStep(), new DecideStep(), new AcquireStep(),
        new ReleaseStep(), new VerifyRipsStep(), new NameStep(), new SealStep(), new CommitStep(), new NotifyStep(), new KeepApartStep(),
    });
}
