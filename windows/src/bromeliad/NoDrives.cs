using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromeliad;

/// <summary>RootOptions.Drives false: the operating system reports no drive.</summary>
sealed class NoDeviceMonitor : IDeviceMonitor
{
    public void Start(Action<DeviceEvent> sink) { }

    public void Stop() { }

    public IReadOnlyList<OsDrive> CurrentDrives() => Array.Empty<OsDrive>();
}

/// <summary>RootOptions.Drives false: every drive command is refused (drive.notFound).</summary>
sealed class NoDriveControl : IDriveControl
{
    public Task Eject(string device) => throw Refused(device);

    public Task CloseTray(string device) => throw Refused(device);

    public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => Task.FromResult<string?>(null);

    public DiscContent ProbeContent(string device) => DiscContent.Unknown;

    public ISectorReader OpenRaw(string device) => throw Refused(device);

    static BroFailure Refused(string device) =>
        new(new BroMessage(MessageCode.DriveNotFound, Severity.Error, ("driveId", JsonValue.Of(device))).ToError());
}

/// <summary>RootOptions.Drives false: MakeMKV as it is, but its drive scan finds none and never starts makemkvcon.</summary>
sealed class NoDriveScan : IMakemkv
{
    readonly IMakemkv _makemkv;

    public NoDriveScan(IMakemkv makemkv) { _makemkv = makemkv; }

    public Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel) => Task.FromResult<IReadOnlyList<MakemkvDrive>>(Array.Empty<MakemkvDrive>());

    public Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
        _makemkv.Listing(source, invocation, sink, cancel);

    public Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
        _makemkv.Rip(source, title, destination, invocation, sink, cancel);

    public Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
        _makemkv.Backup(source, decrypt, destination, invocation, sink, cancel);
}
