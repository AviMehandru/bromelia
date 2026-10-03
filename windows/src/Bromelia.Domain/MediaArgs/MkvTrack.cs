namespace Bromelia.Domain;

/// <summary>A track of an MKV as mkvmerge -J lists it: its id and type (video, audio, subtitles).</summary>
public sealed record MkvTrack(int Id, string Type);
