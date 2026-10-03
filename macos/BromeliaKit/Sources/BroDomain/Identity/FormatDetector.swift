/// Which format a disc, ISO or backup is.
public enum FormatDetector {
    /// From the listing (MakeMKV's type; a Blu-ray whose video is 2160p or HEVC is UHD), else from a backup's
    /// structure (VIDEO_TS is a DVD; BDMV/index.bdmv starts with INDX0300 on UHD discs, INDX0200 on others), else from
    /// the drive's flags; unknown otherwise.
    public static func detect(_ listing: Listing?, flags: DiscFlags?, indexBdmv: String?, hasVideoTs: Bool = false) -> DiscFormat {
        if let listing {
            let t = MessageCatalog.asciiLower(listing.typeText)
            if t.contains("blu") { return isUhd(listing) ? .uhd : .bluray }
            if t.contains("hd") { return .hddvd }
            if t.contains("dvd") { return .dvd }
            if isUhd(listing) { return .uhd }
        }
        if hasVideoTs { return .dvd }
        if let index = indexBdmv, index.hasPrefix("INDX") { return index.hasPrefix("INDX0300") ? .uhd : .bluray }
        if let f = flags {
            if f.blurayFiles { return .bluray }
            if f.hdDvdFiles { return .hddvd }
            if f.dvdFiles { return .dvd }
        }
        return .unknown
    }

    /// DVD, BR, 4K, HDDVD or DISC; with an `e` suffix when the backup isn't decrypted.
    public static func code(_ format: DiscFormat, encrypted: Bool) -> FormatCode {
        let b: String
        switch format {
        case .dvd: b = "DVD"
        case .bluray: b = "BR"
        case .uhd: b = "4K"
        case .hddvd: b = "HDDVD"
        case .unknown: b = "DISC"
        }
        return FormatCode(encrypted ? b + "e" : b)
    }

    static func isUhd(_ listing: Listing) -> Bool {
        listing.titles.contains { t in
            t.tracks.contains { tr in
                guard tr.kind == .video else { return false }
                let size = tr.attributes[AttributeId.videoSize.rawValue] ?? ""
                let codec = MessageCatalog.asciiLower((tr.attributes[AttributeId.codecId.rawValue] ?? "") + " " + (tr.attributes[AttributeId.codecShort.rawValue] ?? ""))
                return size.contains("2160") || size.contains("3840") || codec.contains("hevc") || codec.contains("mpegh")
            }
        }
    }
}
