/// A MSG line: code, flags, parameter count, MakeMKV's English text, its format and parameters.
public struct RobotMessage: Sendable, Equatable {
    public var code: Int
    public var flags: Int
    public var count: Int
    public var text: String
    public var format: String
    public var params: [String]

    public init(code: Int, flags: Int, count: Int = 0, text: String, format: String = "", params: [String] = []) {
        self.code = code
        self.flags = flags
        self.count = count
        self.text = text
        self.format = format
        self.params = params
    }
}
