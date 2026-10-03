/// A decoded DVD VM jump, with its condition as text (empty when it has none).
public enum Jump: Sendable, Equatable {
    /// To a chapter: of VTS title `titleNumber` (JumpVTS_PTT) or of the current title (LinkPTTN, titleNumber nil).
    case ptt(titleNumber: Int?, chapter: Int, condition: String)
    /// To a disc title (JumpTT).
    case title(number: Int, condition: String)
}
