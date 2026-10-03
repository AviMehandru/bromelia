/// A HandBrake step: HandBrakeCLI (empty = found by ToolLocator), the preset and the file it comes from, the output
/// path (step tokens; never replaces a file) and more arguments.
public struct HandBrakeSettings: Sendable, Equatable {
    public var executable = ""
    public var preset = "H.265 MKV 1080p30"
    public var presetFile = ""
    public var outputPath = "{outputDir}/Encoded/{stem}.mkv"
    public var extraArguments = ""

    public init() {}
}
