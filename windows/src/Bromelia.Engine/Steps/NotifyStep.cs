using System;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Step 18 (plan §20.1), a placeholder in Phase 3 (owner, 2026-10-21): it stays in Plan's list and only reports
/// success; the runner publishes JobFinished. The outbox and the sending come with NotificationService in Phase 5. Data:
/// {}.</summary>
public sealed class NotifyStep : IStep
{
    public StepKind Kind => StepKind.Notify;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None;

    public Task<StepReport> Run(StepContext context) => Task.FromResult(StepReport.Done(JsonValue.Of(Array.Empty<(string, JsonValue)>())));
}
