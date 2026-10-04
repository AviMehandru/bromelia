import BroDomain
import BroFoundation
import BroPorts

/// What a run needs besides its source: the settings to isolate, the switches (the profile path comes from the lease),
/// how long it may stay silent, and the transcript file (the job's makemkv.txt).
public struct MakemkvInvocation: Sendable, Equatable {
    public var settings: MakemkvRunSettings
    public var options: MakemkvOptions
    public var stallTimeout: Duration?
    public var transcript: String?

    public init(settings: MakemkvRunSettings, options: MakemkvOptions, stallTimeout: Duration? = nil, transcript: String? = nil) {
        self.settings = settings
        self.options = options
        self.stallTimeout = stallTimeout
        self.transcript = transcript
    }
}
