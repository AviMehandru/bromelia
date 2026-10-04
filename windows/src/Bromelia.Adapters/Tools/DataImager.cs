using System;
using System.IO;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>Data discs to ISO images (plan §10.1; shared/fixtures/adapters/data-imager.cases.json): every sector of
/// DriveControl.OpenRaw, 1 MiB at a time, into a hidden file that becomes the image only once every sector was read.
/// A read error names the first byte that can't be read (other.readError).</summary>
public sealed class DataImager
{
    const int Sector = 2048;
    const int Chunk = 512;
    const int Max = 10000;

    readonly IDriveControl _drives;
    readonly IFileSystem _fs;

    public DataImager(IDriveControl drives, IFileSystem fs)
    {
        _drives = drives;
        _fs = fs;
    }

    /// <summary>Copies the disc in <paramref name="device"/> to <paramref name="destIso"/> (which mustn't exist; its folder
    /// must) and returns the bytes copied.</summary>
    public Task<long> Copy(string device, string destIso, IRunSink sink, CancellationToken cancel) => Task.Run(() =>
    {
        if (_fs.Exists(destIso))
            throw new BroFailure(new BroMessage(MessageCode.FsAlreadyExists, Severity.Error, ("path", JsonValue.Of(destIso))).ToError());
        if (cancel.IsCancelled) throw Cancelled();
        var reader = _drives.OpenRaw(device);
        var folder = Path.GetDirectoryName(destIso) ?? ".";
        var part = Path.Combine(folder, "." + Path.GetFileName(destIso) + ".part-" + Guid.NewGuid().ToString("N")[..8]);
        var done = false;
        try
        {
            long total = reader.SectorCount(), copied = 0;
            using (var file = new FileStream(part, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20))
            {
                while (copied < total)
                {
                    if (cancel.IsCancelled) throw Cancelled();
                    var count = (int)Math.Min(Chunk, total - copied);
                    var data = ReadChunk(reader, copied, count);
                    file.Write(data, 0, data.Length);
                    copied += count;
                    var share = (int)(copied * Max / total);
                    sink.Event(new RobotEvent.ProgressValue(share, share, Max));
                }
                file.Flush(true);
            }
            if (cancel.IsCancelled) throw Cancelled();
            _fs.Rename(part, destIso);
            _fs.SyncDirectory(folder);
            done = true;
            return copied * Sector;
        }
        finally
        {
            reader.Close();
            if (!done)
                try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    });

    /// <summary>A chunk; when it can't be read whole, its sectors one by one, so the error names the first bad byte (a
    /// short read is an error too: the image is never shorter than the disc).</summary>
    static byte[] ReadChunk(ISectorReader reader, long sector, int count)
    {
        try
        {
            var whole = reader.Read(sector, count);
            if (whole.Length == count * Sector) return whole;
        }
        catch (BroFailure) { }
        var data = new byte[count * Sector];
        for (int i = 0; i < count; i++)
        {
            byte[] one;
            try { one = reader.Read(sector + i, 1); }
            catch (BroFailure f)
            {
                var reason = f.Error.Params["reason"]?.AsString ?? f.Error.Code;
                throw new BroFailure(new BroMessage(MessageCode.OtherReadError, Severity.Error, ("offset", JsonValue.Of((sector + i) * Sector)),
                    ("reason", JsonValue.Of(reason))).ToError());
            }
            if (one.Length != Sector)
                throw new BroFailure(new BroMessage(MessageCode.OtherReadError, Severity.Error, ("offset", JsonValue.Of((sector + i) * Sector)),
                    ("reason", JsonValue.Of("short read"))).ToError());
            one.CopyTo(data, i * Sector);
        }
        return data;
    }

    static BroFailure Cancelled() => new(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
}
