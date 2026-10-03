namespace Bromelia.Domain;

/// <summary>A drive as MakeMKV reports it (a DRV line): its index for <c>disc:N</c>, state, disc flags,
/// identification (model, firmware, usually serial), disc label and OS device.</summary>
public sealed record MakemkvDrive(int Index, DriveState State, DiscFlags Flags, string Identification, string Label, string Device)
{
    public static MakemkvDrive? From(RobotEvent @event) => @event is RobotEvent.Drive d
        ? new MakemkvDrive(d.Index, StateFromRaw(d.State), new DiscFlags(d.Flags), d.Identification, d.Label, d.Device)
        : null;

    /// <summary>The drive is there: its state isn't noDrive, and it has a name or a device.</summary>
    public static bool IsPresent(MakemkvDrive drive) =>
        drive.State != DriveState.NoDrive && !(drive.Identification.Length == 0 && drive.Device.Length == 0);

    private static DriveState StateFromRaw(int raw) => raw switch
    {
        0 => DriveState.EmptyClosed,
        1 => DriveState.EmptyOpen,
        2 => DriveState.Inserted,
        3 => DriveState.Loading,
        257 => DriveState.Unmounting,
        _ => DriveState.NoDrive,
    };
}
