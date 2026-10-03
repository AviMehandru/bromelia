/// A wall-clock time in the user's time zone, for the date tokens (the adapters make it from the clock).
public struct LocalTime: Sendable, Equatable {
    public var year, month, day, hour, minute, second: Int

    public init(year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int) {
        self.year = year
        self.month = month
        self.day = day
        self.hour = hour
        self.minute = minute
        self.second = second
    }
}
