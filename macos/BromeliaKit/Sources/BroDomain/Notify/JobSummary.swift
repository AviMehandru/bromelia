/// What a job notification says: the mode, what was ripped (the identified name, else the disc label), how many
/// files went where, and the job's error (nil: none).
public struct JobSummary: Sendable, Equatable {
    public var mode: RipMode
    public var what: String
    public var files: Int
    public var path: String
    public var error: BroMessage?

    public init(mode: RipMode, what: String, files: Int, path: String, error: BroMessage? = nil) {
        self.mode = mode
        self.what = what
        self.files = files
        self.path = path
        self.error = error
    }
}
