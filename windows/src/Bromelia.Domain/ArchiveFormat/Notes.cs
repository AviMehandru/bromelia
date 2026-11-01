using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The INCOMPLETE / READ ERRORS note, written into a folder whose files aren't a finished archive.</summary>
public static class Notes
{
    /// <summary>The note's lines: the job and its outcome, a warning that the files are not a finished archive,
    /// the error, the read errors MakeMKV reported, and the log files copied into the folder (<paramref name="logs"/>:
    /// the job's log, then MakeMKV's, when it was copied), then where the job's full log is (<paramref name="fullLog"/>, its
    /// log.jsonl in the data folder; none for a folder no job owns), as today's apps end the note (owner H3b-6).</summary>
    public static List<BroMessage> Render(Id jobId, Outcome outcome, BroError? error, IReadOnlyList<RobotMessage> readErrors, IReadOnlyList<string> logs,
        string? fullLog)
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
        if (fullLog != null) lines.Add(new BroMessage(MessageCode.ArchiveNoteFullLog, Severity.Info, ("path", JsonValue.Of(fullLog))));
        return lines;
    }
}
