import BroFoundation

/// What a job makes of the disc in the drive.
public enum ModeChooser {
    /// A disc with DVD, Blu-ray or HD DVD files, or video content, gets the profile's mode: by format (mode.byFormat)
    /// unless titles were chosen by hand, else mode.default. An audio CD gets audioCD and a data disc dataImage when
    /// the profile's otherDiscs allow it; a blank disc, or another disc that isn't allowed, gets nil (it's left
    /// alone). `profile` may be sparse: missing fields take their defaults.
    public static func mode(_ format: DiscFormat?, flags: DiscFlags, content: DiscContent, profile: Profile, chosenByHand: Bool) -> RipMode? {
        var issues: [Issue] = []
        let p = SchemaWalker.normalize(profile.json, SchemaWalker.def("ProfileFields"), "", fill: true, full: true, &issues)
        if !(flags.dvdFiles || flags.blurayFiles || flags.hdDvdFiles) {
            let other = p["otherDiscs"]
            switch content {
            case .audio: return other?["ripAudioCDs"]?.bool == true ? .audioCD : nil
            case .data: return other?["imageDataDiscs"]?.bool == true ? .dataImage : nil
            case .blank: return nil
            case .video, .unknown: break
            }
        }
        let mode = p["mode"]
        var byFormat: String?
        if !chosenByHand, let format, format != .unknown { byFormat = mode?["byFormat"]?[format.rawValue]?.string }
        return RipMode(rawValue: byFormat ?? mode?["default"]?.string ?? "") ?? .mkv
    }
}
