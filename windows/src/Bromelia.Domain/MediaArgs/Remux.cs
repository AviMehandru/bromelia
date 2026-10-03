using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>mkvmerge arguments that keep only the chosen tracks of a file ripped with every track selected.</summary>
public static class Remux
{
    private static string? ExpectedType(TrackKind k) => k switch
    {
        TrackKind.Video => "video",
        TrackKind.Audio => "audio",
        TrackKind.Subtitle => "subtitles",
        _ => null,
    };

    /// <summary><c>-o output --video-tracks … | --no-video, the same for audio and subtitles, input</c> (a --no-…
    /// only for a type the file has); null when the file's tracks don't match the title's (another count, or a
    /// video, audio or subtitle track of the listing where the file has another type). <paramref name="keep"/>
    /// holds the title's track indexes.</summary>
    public static List<string>? Arguments(IReadOnlyList<MkvTrack> layout, Title title, IReadOnlyCollection<int> keep, string input, string output)
    {
        if (layout.Count != title.Tracks.Count) return null;
        for (int i = 0; i < layout.Count; i++)
            if (ExpectedType(title.Tracks[i].Kind) is { } type && type != layout[i].Type) return null;
        var video = new List<int>();
        var audio = new List<int>();
        var subs = new List<int>();
        for (int i = 0; i < layout.Count; i++)
        {
            if (!System.Linq.Enumerable.Contains(keep, title.Tracks[i].Index)) continue;
            switch (layout[i].Type)
            {
                case "video": video.Add(layout[i].Id); break;
                case "audio": audio.Add(layout[i].Id); break;
                case "subtitles": subs.Add(layout[i].Id); break;
            }
        }
        var args = new List<string> { "-o", output };
        // A type the file has: the tracks kept, or none of them.
        void Add(List<int> kept, string type, string tracks, string none)
        {
            if (kept.Count > 0) args.AddRange(new[] { tracks, string.Join(",", kept) });
            else if (System.Linq.Enumerable.Any(layout, t => t.Type == type)) args.Add(none);
        }
        Add(video, "video", "--video-tracks", "--no-video");
        Add(audio, "audio", "--audio-tracks", "--no-audio");
        Add(subs, "subtitles", "--subtitle-tracks", "--no-subtitles");
        args.Add(input);
        return args;
    }
}
