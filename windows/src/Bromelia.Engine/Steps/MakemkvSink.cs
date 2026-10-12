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
