namespace Bromelia.Domain;

/// <summary>An optical drive as the OS reports it (the DeviceMonitor port): its device, identification
/// (vendor and model, when the OS knows it) and where its disc is mounted.</summary>
public sealed record OsDrive(string Device, string Identification = "", string? MountPath = null);
