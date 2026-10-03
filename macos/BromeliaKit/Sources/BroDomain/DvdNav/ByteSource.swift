/// The one input interface defined in Domain: the files of a VIDEO_TS folder, wherever they are (an ISO, a folder or
/// a disc). Adapters implement it over FileSystem or SectorReader.
public protocol ByteSource {
    /// `length` bytes of `path` (an upper-case file name such as `VTS_01_0.IFO`) from byte `offset`; fewer at the end
    /// of the file, none when it can't be read.
    func read(_ path: String, offset: Int64, length: Int) -> [UInt8]

    /// Every file, by name.
    func files() -> [ByteFile]
}
