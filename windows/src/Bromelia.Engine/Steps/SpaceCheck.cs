using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>Room for a plan on its library's volume (plan §21).</summary>
public static class SpaceCheck
{
    /// <summary>Logs space.report {free, need}, then keeps <paramref name="need"/> plus the margin
    /// (SpaceEstimate.required) on the volume of <paramref name="libraryRoot"/>: space.insufficient fails the job before
    /// anything is written.</summary>
    public static async Task Reserve(StepContext context, string libraryRoot, long need)
    {
        var volume = context.Fs.Volume(libraryRoot);
        context.Log.Log(new BroMessage(MessageCode.SpaceReport, Severity.Info, ("free", JsonValue.Of(volume.FreeBytes)), ("need", JsonValue.Of(need))));
        await context.Space.Reserve(volume.Id, libraryRoot, SpaceEstimate.Required(new Bytes(need)).Count, volume.FreeBytes);
    }
}
