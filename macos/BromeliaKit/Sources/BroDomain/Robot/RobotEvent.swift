/// A parsed line of makemkvcon's robot output.
public enum RobotEvent: Sendable, Equatable {
    /// MSG: a message.
    case message(RobotMessage)
    /// PRGV: progress values; the fractions are current/max and total/max.
    case progressValue(current: Int, total: Int, max: Int)
    /// PRGC: the current (sub-)operation.
    case progressCurrent(code: Int, id: Int, name: String)
    /// PRGT: the total operation.
    case progressTotal(code: Int, id: Int, name: String)
    /// DRV: a drive of `info disc:9999`, as reported (see `MakemkvDrive.from`).
    case drive(index: Int, state: Int, flags: Int, identification: String, label: String, device: String)
    /// TCOUNT: the number of titles.
    case titleCount(count: Int)
    /// CINFO: an attribute of the disc.
    case discInfo(id: Int, code: Int, value: String)
    /// TINFO: an attribute of a title.
    case titleInfo(title: Int, id: Int, code: Int, value: String)
    /// SINFO: an attribute of a track (stream) of a title.
    case streamInfo(title: Int, stream: Int, id: Int, code: Int, value: String)
    /// Anything else, as is.
    case raw(text: String)
}
