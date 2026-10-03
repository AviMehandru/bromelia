namespace Bromelia.Domain;

public enum JobKind
{
    VideoDisc,
    AudioCd,
    DataDisc,
    ReadErrorRetry,
    Verify,
    Replicate,
    Parity,
    Repair,
    Rescan,
    RunCommand,
    Transcode,
}
