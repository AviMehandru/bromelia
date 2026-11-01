using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Turns what makemkvcon says into the job's progress and log: every message but debug ones as
/// makemkv.message with its severity, the total progress as the step's fraction (under the given operation, with
/// MakeMKV's current item), and each new total operation as makemkv.titleProgress.</summary>
public sealed class MakemkvSink : IRunSink
{
    readonly IProgressReporter _progress;
    readonly IJobLogger _log;
    readonly BroMessage _operation;
    readonly object _lock = new();
    string? _current;
    string? _total;

    public MakemkvSink(IProgressReporter progress, IJobLogger log, BroMessage operation)
    {
        _progress = progress;
        _log = log;
        _operation = operation;
    }

    /// <summary>MakeMKV starts its debug log (app_ShowDebug, named by message 1004) afresh at every start, so after each run
    /// the steps add it to the job folder's makemkv-debug.txt (SealStep.debugTranscript), after a line "==== &lt;its path&gt;
    /// after $ makemkvcon &lt;command&gt;" (the subcommand: info, mkv, backup), as today's apps do. Nothing when the run named none or it is empty; a file that can't be read or written is
    /// left out (the run's result stands).</summary>
    public static void KeepDebugLog(StepContext context, RunOutcome outcome, string command)
    {
        if (outcome.DebugLog is not { Length: > 0 } path) return;
        var fs = context.Fs;
        try
        {
            if (!fs.Exists(path)) return;
            var text = System.Text.Encoding.UTF8.GetString(fs.Read(path));
            if (text.Length == 0) return;
            var file = context.JobDirectory + "/" + SealStep.DebugTranscript;
            var before = fs.Exists(file) ? System.Text.Encoding.UTF8.GetString(fs.Read(file)) : "";
            if (!fs.Exists(context.JobDirectory)) fs.CreateDirectory(context.JobDirectory, parentsMustExist: false);
            fs.WriteAtomically(file, System.Text.Encoding.UTF8.GetBytes(before + "==== " + path + " after $ makemkvcon " + command + "\n" + text + (text.EndsWith('\n') ? "" : "\n")),
                0x180); // 0600, as the job's other files
        }
        catch (BroFailure)
        {
        }
    }

    public void Event(RobotEvent @event)
    {
        lock (_lock)
        {
            switch (@event)
            {
                case RobotEvent.Message { Value: var m }:
                    var kind = MessageCatalog.Severity(m);
                    if (kind == MessageKind.Debug) return;
                    _log.Log(new BroMessage(MessageCode.MakemkvMessage, kind switch
                    {
                        MessageKind.Error => Severity.Error,
                        MessageKind.Warning => Severity.Warning,
                        _ => Severity.Info,
                    }, ("text", JsonValue.Of(m.Text))));
                    break;
                case RobotEvent.ProgressTotal t:
                    if (t.Name != _total)
                    {
                        _total = t.Name;
                        _log.Log(new BroMessage(MessageCode.MakemkvTitleProgress, Severity.Info, ("name", JsonValue.Of(t.Name))));
                    }
                    break;
                case RobotEvent.ProgressCurrent c:
                    _current = c.Name;
                    break;
                case RobotEvent.ProgressValue v when v.Max > 0:
                    _progress.Report(System.Math.Clamp((double)v.Total / v.Max, 0, 1), _operation, _current);
                    break;
            }
        }
    }
}
