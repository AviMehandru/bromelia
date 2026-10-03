using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Checks a ripped MKV against its title in the disc listing.</summary>
public static class RipCheck
{
    /// <summary>Problems: no tracks, no video track when the title has one, no duration, or a duration outside
    /// <see cref="Tolerance"/>. Notes: more tracks than listed, or a chapter count other than the listing's (one
    /// more is fine: MakeMKV may add a chapter at 00:00).</summary>
    public static RipCheckResult Check(MkvProbe probe, Title title)
    {
        var problems = new List<BroMessage>();
        var notes = new List<BroMessage>();
        var types = probe.Tracks.Select(t => t.Type).ToList();
        if (types.Count == 0) problems.Add(new BroMessage(MessageCode.RipcheckNoTracks));
        if (types.Count > 0 && !types.Contains("video") && title.Tracks.Any(t => t.Kind == TrackKind.Video))
            problems.Add(new BroMessage(MessageCode.RipcheckNoVideo));
        double expected = title.DurationSeconds;
        if (expected > 0)
        {
            if (probe.DurationSeconds is { } d)
            {
                if (Math.Abs(d - expected) > Tolerance(expected).Seconds)
                    problems.Add(new BroMessage(MessageCode.RipcheckDuration, Severity.Info,
                        ("actual", JsonValue.Of(Duration.FormatClock(new Duration(Math.Round(d))))), ("expected", JsonValue.Of(Duration.FormatClock(new Duration(expected))))));
            }
            else problems.Add(new BroMessage(MessageCode.RipcheckNoDuration));
        }
        if (title.Tracks.Count > 0 && types.Count > title.Tracks.Count)
            notes.Add(new BroMessage(MessageCode.RipcheckMoreTracks, Severity.Info, ("have", JsonValue.Of(types.Count)), ("listed", JsonValue.Of(title.Tracks.Count))));
        if (title.Chapters > 1 && (probe.ChapterCount < title.Chapters || probe.ChapterCount > title.Chapters + 1))
            notes.Add(new BroMessage(MessageCode.RipcheckChapters, Severity.Info, ("have", JsonValue.Of(probe.ChapterCount)), ("listed", JsonValue.Of(title.Chapters))));
        return new RipCheckResult(problems, notes);
    }

    /// <summary>The difference allowed between the listed and the actual duration: 5 s or 0.5 %, whichever is
    /// larger.</summary>
    public static Duration Tolerance(double expectedSeconds) => new(Math.Max(5, expectedSeconds * 0.005));
}
