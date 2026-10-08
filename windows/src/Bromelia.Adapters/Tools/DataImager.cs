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
/// A read error names the first byte that can't be read (other.readError); a disc with no sectors is other.emptyDisc,
/// never an empty image. Once the image has its name it is complete: a folder sync that fails after that (some network
/// shares refuse one) is a warning on the result, fs.notSynced, not an error a retry would trip over.</summary>
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
    /// must).</summary>
    public Task<DataImageCopy> Copy(string device, string destIso, IRunSink sink, CancellationToken cancel) => Task.Run(() =>
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
            if (total <= 0) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.OtherEmptyDisc)));
            try
            {
                using var file = new FileStream(part, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20);
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
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // A full disk, as on macOS and Linux: fs.failed for the hidden file, which goes below.
                throw new BroFailure(new BroMessage(MessageCode.FsFailed, Severity.Error, ("operation", JsonValue.Of("write")),
                    ("path", JsonValue.Of(part)), ("reason", JsonValue.Of(e.Message))).ToError());
            }
            if (cancel.IsCancelled) throw Cancelled();
            _fs.Rename(part, destIso);
            done = true;
            BroMessage? warning = null;
            try { _fs.SyncDirectory(folder); }
            catch (BroFailure f)
            {
                warning = new BroMessage(MessageCode.FsNotSynced, Severity.Warning, ("path", JsonValue.Of(destIso)),
                    ("reason", f.Error.Params["reason"] ?? JsonValue.Of(f.Error.Code)));
            }
            return new DataImageCopy(copied * Sector, warning);
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
