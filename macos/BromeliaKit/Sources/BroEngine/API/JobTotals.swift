/// A job's counts (dtos.json Job.counts).
public struct JobTotals: Sendable, Equatable {
    public var files: Int
    public var warnings: Int
    public var errors: Int
    public var readErrors: Int

    public init(files: Int, warnings: Int, errors: Int, readErrors: Int) {
        self.files = files
        self.warnings = warnings
        self.errors = errors
        self.readErrors = readErrors
    }
}
