namespace Bromelia.Domain;

/// <summary>Every step a job can run (plan §20.1).</summary>
public enum StepKind
{
    AwaitMedia,
    Probe,
    Reconcile,
    ReadNavigation,
    Identify,
    Plan,
    Decide,
    Acquire,
    Release,
    VerifyRips,
    Transform,
    Name,
    Seal,
    Commit,
    Publish,
    Protect,
    PostProcess,
    Notify,
    RipAudio,
    ImageData,
    MergeAttempts,
    VerifyUnit,
    Replicate,
    Parity,
    Repair,
    Rescan,
    RunCommand,
    Transcode,
}
