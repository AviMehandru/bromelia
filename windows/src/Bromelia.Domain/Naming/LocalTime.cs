namespace Bromelia.Domain;

/// <summary>A wall-clock time in the user's time zone, for the date tokens (the adapters make it from the
/// clock).</summary>
public sealed record LocalTime(int Year, int Month, int Day, int Hour, int Minute, int Second);
