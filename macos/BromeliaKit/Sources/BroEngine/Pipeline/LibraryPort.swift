import BroFoundation

/// What a step may do with a library (plan §12.2), implemented by LibraryService. The commit (§22.3) joins it with
/// that service.
public protocol LibraryPort: Sendable {
    /// library.offline when its marker is missing.
    func require(_ libraryId: String) throws(BroError) -> LibraryHandle

    /// Where the job's files are made: <library>/.bromelia/staging/<job> (the same file system, so the commit is a
    /// rename).
    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String
}
