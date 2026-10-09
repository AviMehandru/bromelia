using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>One run: its outcome, the first key, version or drive notice, the LibreDrive detail, MakeMKV's version,
/// process.noTranscript when its transcript couldn't be written, and what giving its settings back reported
/// (IIsolationLease.Release: makemkv.keyNotRemoved, makemkv.registryNotRestored).</summary>
public sealed record MakemkvRun(
    RunOutcome Outcome,
    MakemkvNotice? Notice,
    string? LibreDrive,
    string? Version,
    BroMessage? TranscriptProblem = null,
    BroMessage? SettingsProblem = null);
