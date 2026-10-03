using System;
using System.Collections.Generic;
using System.Globalization;

namespace Bromelia.Domain;

/// <summary>The values of the template tokens.</summary>
public static class TokenRegistry
{
    /// <summary>The tokens shared by every file of a job: name, kind, format, rip, discLabel, discNumber, season,
    /// part, volumeNumber, disc, volume, type, drive, job, date, time, year, month, day, releaseYear, seasonOr1 and
    /// libraryFolder (Movies / TV Shows). The per-file tokens (episode, episodeNumber, episodeTitle, track) are
    /// empty here; each <see cref="PlannedOutput"/> sets its own.</summary>
    public static Dictionary<string, string> Values(Identity identity, TokenContext context)
    {
        string N(int? v) => v?.ToString(CultureInfo.InvariantCulture) ?? "";
        string Two(int v) => v.ToString("00", CultureInfo.InvariantCulture);
        var t = context.LocalTime;
        return new Dictionary<string, string>
        {
            ["name"] = identity.Name,
            ["kind"] = Foundation.EnumWire.Name(identity.Kind),
            ["format"] = identity.FormatCode.Text,
            ["rip"] = context.Rip,
            ["discLabel"] = LabelParser.SetDescription(identity.Label),
            ["discNumber"] = N(identity.Label.Disc),
            ["season"] = N(identity.Label.Season),
            ["part"] = N(identity.Label.Part),
            ["volumeNumber"] = N(identity.Label.Volume),
            ["disc"] = context.Disc.Length > 0 ? context.Disc : context.Volume,
            ["volume"] = context.Volume,
            ["type"] = Foundation.EnumWire.Name(context.Type),
            ["drive"] = context.Drive,
            ["job"] = context.Job,
            ["date"] = t == null ? "" : $"{t.Year:0000}-{Two(t.Month)}-{Two(t.Day)}",
            ["time"] = t == null ? "" : $"{Two(t.Hour)}-{Two(t.Minute)}-{Two(t.Second)}",
            ["year"] = t == null ? "" : t.Year.ToString("0000", CultureInfo.InvariantCulture),
            ["month"] = t == null ? "" : Two(t.Month),
            ["day"] = t == null ? "" : Two(t.Day),
            ["releaseYear"] = N(context.ReleaseYear),
            ["seasonOr1"] = N(identity.Label.Season ?? 1),
            ["libraryFolder"] = identity.Kind == MediaKind.Tv ? "TV Shows" : "Movies",
            ["episode"] = "",
            ["episodeNumber"] = "",
            ["episodeTitle"] = "",
            ["track"] = "",
        };
    }

    /// <summary>"Title 11" for DVD titles, "Playlist 00800" for Blu-ray playlists, else MakeMKV's title number.</summary>
    public static string TrackLabel(Title title)
    {
        var src = title.SourceFile;
        if (src.EndsWith(".mpls", StringComparison.OrdinalIgnoreCase))
        {
            var file = src.Substring(Math.Max(src.LastIndexOf('/'), src.LastIndexOf('\\')) + 1);
            return "Playlist " + file.Substring(0, file.Length - 5);
        }
        return "Title " + (title.SourceTitleId ?? title.Index).ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>"Episode 007" for episode 7 at width 3.</summary>
    public static string EpisodeLabel(int episode, int width) => "Episode " + episode.ToString(CultureInfo.InvariantCulture).PadLeft(width, '0');
}
