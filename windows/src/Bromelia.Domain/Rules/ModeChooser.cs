using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a job makes of the disc in the drive.</summary>
public static class ModeChooser
{
    /// <summary>A disc with DVD, Blu-ray or HD DVD files, or video content, gets the profile's mode: by format
    /// (mode.byFormat) unless titles were chosen by hand, else mode.default. An audio CD gets audioCD and a data disc
    /// dataImage when the profile's otherDiscs allow it; a blank disc, or another disc that isn't allowed, gets null
    /// (it's left alone). <paramref name="profile"/> may be sparse: missing fields take their defaults.</summary>
    public static RipMode? Mode(DiscFormat? format, DiscFlags flags, DiscContent content, Profile profile, bool chosenByHand)
    {
        var p = SchemaWalker.Normalize(profile.Json, SchemaWalker.Def("ProfileFields"), "", true, new System.Collections.Generic.List<Issue>(), full: true);
        bool videoFiles = flags.DvdFiles || flags.BlurayFiles || flags.HdDvdFiles;
        if (!videoFiles)
        {
            var other = p["otherDiscs"]!;
            switch (content)
            {
                case DiscContent.Audio: return other["ripAudioCDs"]?.AsBool == true ? RipMode.AudioCD : null;
                case DiscContent.Data: return other["imageDataDiscs"]?.AsBool == true ? RipMode.DataImage : null;
                case DiscContent.Blank: return null;
            }
        }
        var mode = p["mode"]!;
        var byFormat = !chosenByHand && format is { } f and not DiscFormat.Unknown ? mode["byFormat"]?[EnumWire.Name(f)]?.AsString : null;
        return EnumWire.Parse<RipMode>(byFormat ?? mode["default"]?.AsString) ?? RipMode.Mkv;
    }
}
