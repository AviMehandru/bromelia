using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The INCOMPLETE / READ ERRORS note, written into a folder whose files aren't a finished archive.</summary>
public static class Notes
{
    /// <summary>The note's lines: the job and its outcome, a warning that the files are not a finished archive,
    /// the error, the read errors MakeMKV reported, and the log files copied into the folder (<paramref name="logs"/>:
    /// the job's log, then MakeMKV's, when it was copied).</summary>
    public static List<BroMessage> Render(Id jobId, Outcome outcome, BroError? error, IReadOnlyList<RobotMessage> readErrors, IReadOnlyList<string> logs)
    {
        var lines = new List<BroMessage>
        {
            new(MessageCode.ArchiveNoteTitle, Severity.Info, ("jobId", JsonValue.Of(jobId.Value)), ("outcome", JsonValue.Of(EnumWire.Name(outcome)))),
            new(MessageCode.ArchiveNoteNotFinished, Severity.Info),
        };
        if (error != null)
            lines.Add(new BroMessage(MessageCode.Parse(error.Code) ?? MessageCode.InternalUnexpected, error.Params, Severity.Error));
        if (readErrors.Count > 0)
        {
            lines.Add(new BroMessage(MessageCode.ArchiveNoteReadErrors, Severity.Info));
            foreach (var e in readErrors) lines.Add(new BroMessage(MessageCode.MakemkvMessage, Severity.Error, ("text", JsonValue.Of(e.Text))));
        }
        if (logs.Count > 0)
            lines.Add(new BroMessage(MessageCode.ArchiveNoteLogs, Severity.Info, ("log", JsonValue.Of(logs[0])),
                ("hasMakemkv", JsonValue.Of(logs.Count > 1)), ("makemkvLog", JsonValue.Of(logs.Count > 1 ? logs[1] : ""))));
        return lines;
    }
}
