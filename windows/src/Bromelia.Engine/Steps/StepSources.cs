using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What the video-disc steps share about a job's source: its request details, where makemkvcon opens it, the
/// mode it would get before Plan, and the disc's label before there is a listing.</summary>
public static class StepSources
{
    /// <summary>The job's DiscRequest; internal.unexpected when it names no source.</summary>
    public static DiscRequest Request(JobSnapshot job) => DiscRequest.Decode(job.Request)
        ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected), ("detail", JsonValue.Of("the job has no source"))));

    /// <summary>A drive's location now; drive.notFound when it isn't connected.</summary>
    public static DriveLocation Drive(IDriveLookup lookup, string driveId) => lookup.Find(driveId)
        ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.DriveNotFound), ("driveId", JsonValue.Of(driveId))));

    /// <summary>Where makemkvcon opens the source: disc:N for a drive (drive.notFound when it isn't connected or MakeMKV
    /// hasn't numbered it), the image, or the disc a folder or file belongs to (SourceResolver).</summary>
    public static MakemkvSource Makemkv(DiscSource source, IDriveLookup lookup) => source switch
    {
        DiscSource.Drive d => Drive(lookup, d.DriveId) is { MakemkvIndex: { } index } at
            ? new MakemkvSource.Drive(index, at.Device)
            : throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.DriveNotFound), ("driveId", JsonValue.Of(d.DriveId)))),
        DiscSource.Iso i => new MakemkvSource.Iso(i.Path),
        DiscSource.Folder f => SourceResolver.Source(f.Path, true),
        DiscSource.File f => SourceResolver.Source(f.Path, false),
        _ => throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected))),
    };

    /// <summary>The profile before Plan: the drive entry's, with no facts yet but whether the rip is automatic.</summary>
    public static EffectiveProfile Profile(StepContext context, string? configId) =>
        ProfileResolver.Resolve(context.Config, configId, new RuleFacts("", "", "", Automatic: context.Job.Request.Automatic), null);

    /// <summary>The mode before Plan: the request's (options.mode), else the profile's mode.default.</summary>
    public static RipMode BaseMode(JobSnapshot job, EffectiveProfile profile) =>
        EnumWire.Parse<RipMode>(job.Request.Options?["mode"]?.AsString) ?? EnumWire.Parse<RipMode>(profile.Profile["mode"]?["default"]?.AsString) ?? RipMode.Mkv;

    /// <summary>The disc's label before a listing names it: the drive's disc label, an image's name without its
    /// extension, or the folder's (or file's folder's) name.</summary>
    public static string Label(DiscSource source, DriveLocation? drive)
    {
        static string Last(string p)
        {
            p = p.TrimEnd('/', '\\');
            return p.Substring(p.LastIndexOfAny(new[] { '/', '\\' }) + 1);
        }
        switch (source)
        {
            case DiscSource.Drive:
                return drive?.Label ?? "";
            case DiscSource.Iso i:
            {
                var name = Last(i.Path);
                int dot = name.LastIndexOf('.');
                return dot > 0 ? name.Substring(0, dot) : name;
            }
            case DiscSource.Folder f:
                return Last(f.Path);
            case DiscSource.File f:
                return SourceResolver.Source(f.Path, false) is MakemkvSource.File root ? Last(root.Path) : Last(f.Path);
            default:
                return "";
        }
    }
}
