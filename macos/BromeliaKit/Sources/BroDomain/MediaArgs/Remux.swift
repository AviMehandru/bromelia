/// mkvmerge arguments that keep only the chosen tracks of a file ripped with every track selected.
public enum Remux {
    static func expectedType(_ k: TrackKind) -> String? {
        switch k {
        case .video: return "video"
        case .audio: return "audio"
        case .subtitle: return "subtitles"
        default: return nil
        }
    }

    /// `-o output --video-tracks … | --no-video, the same for audio and subtitles, input` (a --no-… only for a type
    /// the file has); nil when the file's tracks don't match the title's (another count, or a video, audio or
    /// subtitle track of the listing where the file has another type). `keep` holds the title's track indexes.
    public static func arguments(_ layout: [MkvTrack], title: Title, keep: Set<Int>, input: String, output: String) -> [String]? {
        guard layout.count == title.tracks.count else { return nil }
        for (i, t) in layout.enumerated() {
            if let type = expectedType(title.tracks[i].kind), type != t.type { return nil }
        }
        var video: [Int] = [], audio: [Int] = [], subs: [Int] = []
        for (i, t) in layout.enumerated() where keep.contains(title.tracks[i].index) {
            switch t.type {
            case "video": video.append(t.id)
            case "audio": audio.append(t.id)
            case "subtitles": subs.append(t.id)
            default: break
            }
        }
        var args = ["-o", output]
        // A type the file has: the tracks kept, or none of them.
        func add(_ kept: [Int], _ type: String, _ tracks: String, _ none: String) {
            if !kept.isEmpty {
                args += [tracks, kept.map(String.init).joined(separator: ",")]
            } else if layout.contains(where: { $0.type == type }) {
                args.append(none)
            }
        }
        add(video, "video", "--video-tracks", "--no-video")
        add(audio, "audio", "--audio-tracks", "--no-audio")
        add(subs, "subtitles", "--subtitle-tracks", "--no-subtitles")
        args.append(input)
        return args
    }
}
