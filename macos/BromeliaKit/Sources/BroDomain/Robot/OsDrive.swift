/// An optical drive as the OS reports it (the DeviceMonitor port): its device, identification (vendor and
/// model, when the OS knows it) and where its disc is mounted.
public struct OsDrive: Sendable, Equatable {
    public var device: String
    public var identification: String
    public var mountPath: String?

    public init(device: String, identification: String = "", mountPath: String? = nil) {
        self.device = device
        self.identification = identification
        self.mountPath = mountPath
    }
}
