namespace Bromelia.Domain;

/// <summary>A parsed line of makemkvcon's robot output.</summary>
public abstract record RobotEvent
{
    /// <summary>MSG: a message.</summary>
    public sealed record Message(RobotMessage Value) : RobotEvent;
    /// <summary>PRGV: progress values; the fractions are Current/Max and Total/Max.</summary>
    public sealed record ProgressValue(int Current, int Total, int Max) : RobotEvent;
    /// <summary>PRGC: the current (sub-)operation.</summary>
    public sealed record ProgressCurrent(int Code, int Id, string Name) : RobotEvent;
    /// <summary>PRGT: the total operation.</summary>
    public sealed record ProgressTotal(int Code, int Id, string Name) : RobotEvent;
    /// <summary>DRV: a drive of <c>info disc:9999</c>, as reported (see <see cref="MakemkvDrive.From"/>).</summary>
    public sealed record Drive(int Index, int State, int Flags, string Identification, string Label, string Device) : RobotEvent;
    /// <summary>TCOUNT: the number of titles.</summary>
    public sealed record TitleCount(int Count) : RobotEvent;
    /// <summary>CINFO: an attribute of the disc.</summary>
    public sealed record DiscInfo(int Id, int Code, string Value) : RobotEvent;
    /// <summary>TINFO: an attribute of a title.</summary>
    public sealed record TitleInfo(int Title, int Id, int Code, string Value) : RobotEvent;
    /// <summary>SINFO: an attribute of a track (stream) of a title.</summary>
    public sealed record StreamInfo(int Title, int Stream, int Id, int Code, string Value) : RobotEvent;
    /// <summary>Anything else, as is.</summary>
    public sealed record Raw(string Text) : RobotEvent;
}
