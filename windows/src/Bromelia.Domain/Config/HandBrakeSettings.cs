namespace Bromelia.Domain;

/// <summary>A HandBrake step: HandBrakeCLI (empty = found by ToolLocator), the preset and the file it comes from,
/// the output path (step tokens; never replaces a file) and more arguments.</summary>
public sealed record HandBrakeSettings(
    string Executable = "",
    string Preset = "H.265 MKV 1080p30",
    string PresetFile = "",
    string OutputPath = "{outputDir}/Encoded/{stem}.mkv",
    string ExtraArguments = "");
