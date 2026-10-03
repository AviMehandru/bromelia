using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Whether a backup looks like a disc: a folder with a BDMV, VIDEO_TS or HVDVD_TS structure, or an ISO
/// image (an ISO 9660 or UDF volume descriptor at byte 32769).</summary>
public static class BackupStructure
{
    /// <summary>What is wrong with the backup called <paramref name="name"/>, or null. <paramref name="entries"/>
    /// lists the folder's files as relative paths (a trailing / is an empty folder; null: not a folder);
    /// <paramref name="isoHeader"/> is the file's bytes from 32769 (at most 5; null: not a file).</summary>
    public static BroMessage? Problem(string name, bool iso, IReadOnlyList<string>? entries, byte[]? isoHeader)
    {
        BroMessage Named(MessageCode code) => new(code, Severity.Info, ("name", JsonValue.Of(name)));
        if (iso)
        {
            if (entries != null) return Named(MessageCode.StructureIsFolder);
            if (isoHeader == null) return Named(MessageCode.StructureNotCreated);
            var id = isoHeader.Length >= 5 ? System.Text.Encoding.ASCII.GetString(isoHeader, 0, 5) : "";
            return id is "CD001" or "BEA01" ? null : Named(MessageCode.StructureNotImage);
        }
        if (entries == null) return Named(isoHeader != null ? MessageCode.StructureNotFolder : MessageCode.StructureNotCreated);
        bool Has(string prefix) => entries.Any(e => e.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
        if (Has("BDMV/")) return Has("BDMV/index.bdmv") ? null : new BroMessage(MessageCode.StructureBdmvIndexMissing);
        if (Has("VIDEO_TS/")) return Has("VIDEO_TS/VIDEO_TS.IFO") ? null : new BroMessage(MessageCode.StructureVideoTsIfoMissing);
        if (Has("HVDVD_TS/")) return null;
        return new BroMessage(MessageCode.StructureNoDiscFolder);
    }
}
