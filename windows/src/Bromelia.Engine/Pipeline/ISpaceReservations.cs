using System.Threading.Tasks;

namespace Bromelia.Engine;

/// <summary>How a step keeps room on a volume for its job (plan §21), implemented by the runner over the Scheduler's
/// reservations (owner, 2026-10-18): two rips on one volume count each other's space. What a job keeps goes back when
/// it finishes.</summary>
public interface ISpaceReservations
{
    /// <summary>Keeps <paramref name="need"/> bytes on <paramref name="volume"/> for the job, replacing what it kept there.
    /// space.insufficient {path, need, free} when <paramref name="free"/> minus what other jobs keep there is less.</summary>
    Task Reserve(string volume, string path, long need, long free);
}
