/// mkvmerge arguments that split a file at chapters without re-encoding.
public enum Split {
    /// `-o output --split chapters:8,15,… input`; mkvmerge numbers the parts in `output` (-001, -002, …).
    public static func arguments(_ chapters: [Int], input: String, output: String) -> [String] {
        ["-o", output, "--split", "chapters:" + chapters.map(String.init).joined(separator: ","), input]
    }
}
