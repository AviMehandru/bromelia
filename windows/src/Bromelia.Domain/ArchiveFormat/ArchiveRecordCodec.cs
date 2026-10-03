using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Reads archive records (versions 2 and 3) and writes version 3.</summary>
public static class ArchiveRecordCodec
{
    /// <summary>The key order of each object of a version 3 record (archive-record-3.json); keys it doesn't name
    /// follow in the order they had.</summary>
    private static readonly Dictionary<string, string[]> KeyOrder = new()
    {
        [""] = new[] { "format", "version", "unit", "status", "name", "kind", "work", "disc", "physical", "acquisition", "job", "titles", "episodes", "files", "attempts", "parity", "counts", "problems", "logs" },
        ["unit"] = new[] { "id", "short", "library" },
        ["work"] = new[] { "provider", "title", "year", "tmdbId", "imdbId", "chosen" },
        ["disc"] = new[] { "label", "volumeName", "type", "format", "formatCode", "encrypted", "season", "part", "volume", "disc", "set", "fingerprint", "libreDrive" },
        ["physical"] = new[] { "barcode", "location" },
        ["acquisition"] = new[] { "mode", "rip", "source", "drive", "profile", "rules", "makemkv", "bromelia", "tools" },
        ["acquisition.drive"] = new[] { "id", "name", "model" },
        ["job"] = new[] { "id", "startedAt", "finishedAt" },
        ["titles[]"] = new[] { "index", "sourceTitleId", "sourceFile", "duration", "durationSeconds", "chapters", "sizeBytes", "segmentMap", "readErrors" },
        ["episodes[]"] = new[] { "file", "episode", "sourceTitleId", "firstChapter", "lastChapter", "title" },
        ["files[]"] = new[] { "path", "size", "sha256", "role", "title" },
        ["attempts[]"] = new[] { "job", "drive", "startedAt", "titles", "kept" },
        ["parity"] = new[] { "percent", "files" },
        ["counts"] = new[] { "warnings", "errors" },
        ["problems[]"] = new[] { "code", "params", "text", "readError" },
    };

    /// <summary>A record of version 2 or 3; null when the bytes aren't one (not JSON, another format, another
    /// version).</summary>
    public static ArchiveRecord? Decode(byte[] bytes)
    {
        if (JsonValue.Parse(bytes) is not JsonValue.Object doc || doc["format"]?.AsString != "bromelia-archive") return null;
        var version = doc["version"]?.AsInteger;
        if (version is not (2 or 3)) return null;
        var disc = doc["disc"];
        int? I(JsonValue? v) => v?.AsInteger is { } n ? (int)n : null;
        var episodes = (doc["episodes"]?.AsArray ?? new List<JsonValue>()).Select(e => I(e["episode"])).Where(n => n != null).Select(n => n!.Value).ToList();
        var files = (doc["files"]?.AsArray ?? new List<JsonValue>())
            .Where(f => f["path"]?.AsString != null && f["sha256"]?.AsString != null)
            .Select(f => new SumEntry(f["path"]!.AsString!, f["sha256"]!.AsString!)).ToList();
        return new ArchiveRecord((int)version.Value, doc, doc["status"]?.AsString ?? "", doc["name"]?.AsString ?? "", doc["kind"]?.AsString ?? "",
            disc?["label"]?.AsString ?? "", disc?["volumeName"]?.AsString ?? "", I(disc?["season"]), I(disc?["part"]), I(disc?["volume"]),
            I(disc?["disc"]), disc?["fingerprint"]?.AsString, version == 3 ? doc["unit"]?["id"]?.AsString : null, episodes, files);
    }

    /// <summary>A version 3 record: canonical JSON with the keys in archive-record-3.json's order.</summary>
    public static byte[] EncodeV3(ArchiveRecord record)
    {
        if (record.Version != 3) throw new ArgumentException("only version 3 records are written", nameof(record));
        return JsonValue.EncodeCanonical(Ordered(record.Document, ""));
    }

    /// <summary>The record as an archived TV disc for EpisodeContinuation: status success or errors, kind tv; the
    /// label's title from the volume name (else the label); the highest episode number.</summary>
    public static ArchivedDisc? ArchivedDisc(ArchiveRecord record, string folder)
    {
        if (record.Status is not ("success" or "errors") || record.Kind != "tv") return null;
        var label = LabelParser.Parse(record.VolumeName.Length > 0 ? record.VolumeName : record.Label);
        int? last = record.Episodes.Count > 0 ? record.Episodes.Max() : null;
        return new ArchivedDisc(record.Name, label.Title, record.Season, record.Part, record.Volume, record.Disc, last, folder);
    }

    private static JsonValue Ordered(JsonValue v, string path)
    {
        switch (v)
        {
            case JsonValue.Array a:
                return new JsonValue.Array(a.Items.Select(i => Ordered(i, path + "[]")).ToList());
            case JsonValue.Object o:
                var members = o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Ordered(m.Value, (path.Length > 0 ? path + "." : "") + m.Key))).ToList();
                if (!KeyOrder.TryGetValue(path, out var order)) return new JsonValue.Object(members);
                var sorted = members.OrderBy(m => Array.IndexOf(order, m.Key) is var i && i >= 0 ? i : order.Length).ToList(); // stable
                return new JsonValue.Object(sorted);
            default:
                return v;
        }
    }
}
