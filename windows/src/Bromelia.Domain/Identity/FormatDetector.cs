using System;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>Which format a disc, ISO or backup is.</summary>
public static class FormatDetector
{
    /// <summary>From the listing (MakeMKV's type; a Blu-ray whose video is 2160p or HEVC is UHD), else from a
    /// backup's structure (VIDEO_TS is a DVD; BDMV/index.bdmv starts with INDX0300 on UHD discs, INDX0200 on
    /// others), else from the drive's flags; unknown otherwise.</summary>
    public static DiscFormat Detect(Listing? listing, DiscFlags? flags, string? indexBdmv, bool hasVideoTs = false)
    {
        if (listing != null)
        {
            var t = MessageCatalog.AsciiLower(listing.TypeText);
            if (t.Contains("blu")) return IsUhd(listing) ? DiscFormat.Uhd : DiscFormat.Bluray;
            if (t.Contains("hd")) return DiscFormat.Hddvd;
            if (t.Contains("dvd")) return DiscFormat.Dvd;
            if (IsUhd(listing)) return DiscFormat.Uhd;
        }
        if (hasVideoTs) return DiscFormat.Dvd;
        if (indexBdmv != null && indexBdmv.StartsWith("INDX", StringComparison.Ordinal))
            return indexBdmv.StartsWith("INDX0300", StringComparison.Ordinal) ? DiscFormat.Uhd : DiscFormat.Bluray;
        if (flags is { } f)
        {
            if (f.BlurayFiles) return DiscFormat.Bluray;
            if (f.HdDvdFiles) return DiscFormat.Hddvd;
            if (f.DvdFiles) return DiscFormat.Dvd;
        }
        return DiscFormat.Unknown;
    }

    /// <summary>DVD, BR, 4K, HDDVD or DISC; with an <c>e</c> suffix when the backup isn't decrypted.</summary>
    public static FormatCode Code(DiscFormat format, bool encrypted)
    {
        var b = format switch
        {
            DiscFormat.Dvd => "DVD",
            DiscFormat.Bluray => "BR",
            DiscFormat.Uhd => "4K",
            DiscFormat.Hddvd => "HDDVD",
            _ => "DISC",
        };
        return new FormatCode(encrypted ? b + "e" : b);
    }

    private static bool IsUhd(Listing listing) =>
        listing.Titles.Any(t => t.Tracks.Any(tr =>
        {
            if (tr.Kind != TrackKind.Video) return false;
            var size = tr.Attributes.TryGetValue((int)AttributeId.VideoSize, out var v) ? v : "";
            var codec = MessageCatalog.AsciiLower((tr.Attributes.TryGetValue((int)AttributeId.CodecId, out var c) ? c : "") + " "
                + (tr.Attributes.TryGetValue((int)AttributeId.CodecShort, out var cs) ? cs : ""));
            return size.Contains("2160") || size.Contains("3840") || codec.Contains("hevc") || codec.Contains("mpegh");
        }));
}
