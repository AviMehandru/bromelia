using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The active problems every UI shows until they are cleared (plan §11.1), on the executor; a minimal one (owner,
/// 2026-10-12). In memory only: each service raises its problems again when it starts. A problem is keyed by its code
/// and scope; its id is the code, or <c>code:kind:id</c> with a scope. shared/fixtures/engine/problem-service.cases.json.
/// </summary>
public sealed class ProblemService
{
    readonly EventBus _bus;
    readonly IClock _clock;
    readonly List<ProblemDto> _problems = new();

    public ProblemService(EventBus bus, IClock clock)
    {
        _bus = bus;
        _clock = clock;
    }

    /// <summary>The active problems, in the order they were first raised.</summary>
    public IReadOnlyList<ProblemDto> Problems() => _problems.ToList();

    /// <summary>Adds the problem and publishes problemRaised. The same one again does nothing; one with other params,
    /// severity or actions replaces it in place (same id, place and raisedAt) and publishes problemRaised again.</summary>
    public ProblemDto Raise(BroMessage message, ProblemScope? scope = null, IReadOnlyList<ProblemAction>? actions = null)
    {
        var id = Key(message.Code, scope);
        var i = _problems.FindIndex(p => p.Id == id);
        var problem = new ProblemDto(id, message, scope, i >= 0 ? _problems[i].RaisedAt : _clock.Now(), actions ?? new List<ProblemAction>());
        if (i >= 0)
        {
            if (ProblemDto.Json(_problems[i]).Equals(ProblemDto.Json(problem))) return _problems[i];
            _problems[i] = problem;
        }
        else _problems.Add(problem);
        _bus.Publish(EngineEventType.ProblemRaised, JsonValue.Of(("problem", ProblemDto.Json(problem))));
        return problem;
    }

    /// <summary>Removes the problem and publishes problemCleared; nothing when there is none.</summary>
    public void Clear(MessageCode code, ProblemScope? scope = null)
    {
        var id = Key(code, scope);
        if (_problems.RemoveAll(p => p.Id == id) == 0) return;
        _bus.Publish(EngineEventType.ProblemCleared, JsonValue.Of(("problemId", JsonValue.Of(id)), ("code", JsonValue.Of(MessageCode.Wire(code)))));
    }

    static string Key(MessageCode code, ProblemScope? scope) =>
        scope == null ? MessageCode.Wire(code) : MessageCode.Wire(code) + ":" + EnumWire.Name(scope.Kind) + ":" + scope.Id;
}
