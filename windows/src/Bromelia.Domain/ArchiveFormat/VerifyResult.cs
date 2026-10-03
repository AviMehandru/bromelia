using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A unit's check: the verdict, the number of files SHA256SUMS lists, the files that changed, couldn't
/// be read or are missing, the files the folder has that SHA256SUMS doesn't list (not an error: post-processing
/// may add files), and the summary (check.ok / check.damaged, or the error: check.sumsEmpty).</summary>
public sealed record VerifyResult(
    CheckResult Result,
    int Files,
    IReadOnlyList<string> Changed,
    IReadOnlyList<string> Unreadable,
    IReadOnlyList<string> Missing,
    IReadOnlyList<string> Unlisted,
    BroMessage Summary)
{
    /// <summary>From the paths SHA256SUMS lists (<paramref name="sums"/>), what hashing each found
    /// (<paramref name="hashes"/>; a listed path without a verdict counts as missing), and the folder's files
    /// (relative paths, without nested archives: the adapter stops at folders with a SHA256SUMS of their own).
    /// Bromelia's own files at the top of the folder, media-server metadata and hidden files aren't unlisted.</summary>
    public static VerifyResult Compare(IReadOnlyList<string> sums, IReadOnlyDictionary<string, FileVerdict> hashes, IReadOnlyList<string> folderFiles)
    {
        var listed = sums.Distinct().OrderBy(p => p, CodePointOrder.Instance).ToList();
        if (listed.Count == 0)
            return new(CheckResult.Error, 0, new List<string>(), new List<string>(), new List<string>(), new List<string>(),
                new BroMessage(MessageCode.CheckSumsEmpty, Severity.Error));
        FileVerdict V(string p) => hashes.TryGetValue(p, out var v) ? v : FileVerdict.Missing;
        var changed = listed.Where(p => V(p) == FileVerdict.Changed).ToList();
        var unreadable = listed.Where(p => V(p) == FileVerdict.Unreadable).ToList();
        var missing = listed.Where(p => V(p) == FileVerdict.Missing).ToList();
        var set = listed.ToHashSet();
        var unlisted = folderFiles.Where(f =>
        {
            var name = f.Substring(f.LastIndexOf('/') + 1);
            bool top = !f.Contains('/');
            return !set.Contains(f) && !(top && ArchiveFiles.IsOwnFile(name)) && !ArchiveFiles.IsMetadataFile(name)
                   && !f.Split('/').Any(c => c.StartsWith("."));
        }).OrderBy(p => p, CodePointOrder.Instance).ToList();
        bool ok = changed.Count == 0 && unreadable.Count == 0 && missing.Count == 0;
        BroMessage summary;
        if (ok)
            summary = new BroMessage(MessageCode.CheckOk, Severity.Info, ("files", JsonValue.Of(listed.Count)), ("unlisted", JsonValue.Of(unlisted.Count)));
        else
        {
            var parts = new List<JsonValue>();
            if (changed.Count > 0) parts.Add(new BroMessage(MessageCode.CheckPartChanged, Severity.Info, ("count", JsonValue.Of(changed.Count))).ToJson());
            if (unreadable.Count > 0) parts.Add(new BroMessage(MessageCode.CheckPartUnreadable, Severity.Info, ("count", JsonValue.Of(unreadable.Count))).ToJson());
            if (missing.Count > 0) parts.Add(new BroMessage(MessageCode.CheckPartMissing, Severity.Info, ("count", JsonValue.Of(missing.Count))).ToJson());
            summary = new BroMessage(MessageCode.CheckDamaged, Severity.Error, ("parts", JsonValue.Of(parts)), ("files", JsonValue.Of(listed.Count)),
                ("unlisted", JsonValue.Of(unlisted.Count)));
        }
        return new(ok ? CheckResult.Ok : CheckResult.Damaged, listed.Count, changed, unreadable, missing, unlisted, summary);
    }
}
