using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Which titles of a listing to rip (today's TitleSelector).</summary>
public static class TitleRules
{
    /// <summary>Filters, then duplicates (same segment map, length and angle), then the strategy, then
    /// maxTitles. Every title gets a reason.</summary>
    public static Selection Select(Listing listing, TitleSettings rules)
    {
        var titles = listing.Titles;
        var reasons = new Dictionary<int, (bool Selected, BroMessage Reason)>();
        void Reject(Title t, BroMessage why) { if (!reasons.ContainsKey(t.Index)) reasons[t.Index] = (false, why); }
        BroMessage M(MessageCode code, params (string, JsonValue)[] p) => new(code, Severity.Info, p);

        Regex? include = null, exclude = null;
        BroMessage? error = null;
        try { if (rules.IncludePattern.Length > 0) include = new Regex(rules.IncludePattern, RegexOptions.IgnoreCase); }
        catch (ArgumentException) { error = new BroMessage(MessageCode.TitlesInvalidInclude, Severity.Error); }
        try { if (rules.ExcludePattern.Length > 0) exclude = new Regex(rules.ExcludePattern, RegexOptions.IgnoreCase); }
        catch (ArgumentException) { error = new BroMessage(MessageCode.TitlesInvalidExclude, Severity.Error); }

        var candidates = new List<Title>();
        foreach (var t in titles.OrderBy(t => t.Index))
        {
            int d = t.DurationSeconds;
            long mb = t.SizeBytes / 1_000_000;
            string Clock(int s) => Foundation.Duration.FormatClock(new Foundation.Duration(s));
            if (rules.MinDurationSeconds > 0 && d < rules.MinDurationSeconds) { Reject(t, M(MessageCode.TitlesReasonShorterThan, ("duration", JsonValue.Of(Clock(rules.MinDurationSeconds))))); continue; }
            if (rules.MaxDurationSeconds > 0 && d > rules.MaxDurationSeconds) { Reject(t, M(MessageCode.TitlesReasonLongerThan, ("duration", JsonValue.Of(Clock(rules.MaxDurationSeconds))))); continue; }
            if (rules.MinChapters > 0 && t.Chapters < rules.MinChapters) { Reject(t, M(MessageCode.TitlesReasonFewerChapters, ("count", JsonValue.Of(rules.MinChapters)))); continue; }
            if (rules.MaxChapters > 0 && t.Chapters > rules.MaxChapters) { Reject(t, M(MessageCode.TitlesReasonMoreChapters, ("count", JsonValue.Of(rules.MaxChapters)))); continue; }
            if (rules.MinSizeMB > 0 && mb < rules.MinSizeMB) { Reject(t, M(MessageCode.TitlesReasonSmallerThan, ("mb", JsonValue.Of(rules.MinSizeMB)))); continue; }
            if (rules.MaxSizeMB > 0 && mb > rules.MaxSizeMB) { Reject(t, M(MessageCode.TitlesReasonLargerThan, ("mb", JsonValue.Of(rules.MaxSizeMB)))); continue; }
            var hay = MatchText(t);
            if (include != null && !include.IsMatch(hay)) { Reject(t, M(MessageCode.TitlesReasonNoIncludeMatch)); continue; }
            if (exclude != null && exclude.IsMatch(hay)) { Reject(t, M(MessageCode.TitlesReasonExcludeMatch)); continue; }
            if (rules.SkipAlternateAngles && t.Angle is > 1) { Reject(t, M(MessageCode.TitlesReasonAlternateAngle, ("angle", JsonValue.Of(t.Angle.Value)))); continue; }
            candidates.Add(t);
        }

        if (rules.SkipDuplicates)
        {
            var seen = new Dictionary<string, int>();
            candidates = candidates.Where(t =>
            {
                if (t.SegmentMap.Length == 0) return true;
                var key = FormattableString.Invariant($"{t.SegmentMap}|{t.DurationSeconds}|{t.Angle ?? 0}");
                if (seen.TryGetValue(key, out var first)) { Reject(t, M(MessageCode.TitlesReasonDuplicateOf, ("title", JsonValue.Of(first)))); return false; }
                seen[key] = t.Index;
                return true;
            }).ToList();
        }

        var chosen = new List<Title>();
        bool manual = false;
        switch (rules.Strategy)
        {
            case TitleStrategy.All:
                chosen = candidates;
                break;
            case TitleStrategy.Longest:
                int n = Math.Max(1, rules.LongestCount);
                var ranked = candidates.OrderByDescending(t => t.DurationSeconds).ThenByDescending(t => t.Chapters)
                    .ThenByDescending(t => t.SizeBytes).ThenBy(t => t.Index).ToList();
                chosen = ranked.Take(n).ToList();
                foreach (var t in ranked.Skip(n)) Reject(t, M(MessageCode.TitlesReasonNotLongest, ("count", JsonValue.Of(n))));
                break;
            case TitleStrategy.Indices:
                var pattern = IndexPattern.Parse(rules.IndexPattern);
                if (pattern == null)
                {
                    error = new BroMessage(MessageCode.ConfigInvalidIndexPattern, Severity.Error);
                    foreach (var t in candidates) Reject(t, M(MessageCode.TitlesReasonInvalidPattern));
                    break;
                }
                bool bySource = rules.IndexBase == IndexBase.Source;
                int max = bySource ? titles.Select(t => t.SourceTitleId ?? 0).DefaultIfEmpty(0).Max() : titles.Count > 0 ? titles.Max(t => t.Index) : 0;
                foreach (var t in candidates)
                {
                    int v = bySource ? t.SourceTitleId ?? -1 : t.Index;
                    if (pattern.Matches(v, max)) chosen.Add(t);
                    else Reject(t, M(MessageCode.TitlesReasonNotInPattern));
                }
                break;
            case TitleStrategy.Manual:
                manual = true;
                foreach (var t in candidates) reasons[t.Index] = (false, M(MessageCode.TitlesReasonChooseManually));
                break;
        }

        chosen = chosen.OrderBy(t => t.Index).ToList();
        if (rules.MaxTitles > 0 && chosen.Count > rules.MaxTitles)
        {
            foreach (var t in chosen.Skip(rules.MaxTitles)) Reject(t, M(MessageCode.TitlesReasonOverLimit, ("count", JsonValue.Of(rules.MaxTitles))));
            chosen = chosen.Take(rules.MaxTitles).ToList();
        }
        foreach (var t in chosen) reasons[t.Index] = (true, M(MessageCode.TitlesReasonSelected));

        var trace = titles.Select(t => reasons.TryGetValue(t.Index, out var r)
            ? new SelectionTrace(t.Index, r.Selected, r.Reason)
            : new SelectionTrace(t.Index, false, M(MessageCode.TitlesReasonExcluded))).ToList();
        return new Selection(chosen.Select(t => t.Index).ToList(), trace, manual, error);
    }

    /// <summary>The text include and exclude patterns are matched against: name, comment, output file name,
    /// source file name, segment map, <c>#&lt;source id&gt;</c> and duration.</summary>
    internal static string MatchText(Title t) =>
        string.Join(" ", new[] { t.Name, t.Comment, t.OutputFileName, t.SourceFile, t.SegmentMap,
            t.SourceTitleId is { } id ? "#" + id.ToString(CultureInfo.InvariantCulture) : "", t.Duration }.Where(s => s.Length > 0));

    /// <summary>Index patterns such as <c>0,2-4,7-</c>, <c>last</c>, <c>all</c> or <c>*</c>.</summary>
    private sealed class IndexPattern
    {
        private readonly List<(int Low, int High)> _ranges = new(); // High: int.MaxValue = open
        private bool _all, _last;

        public static IndexPattern? Parse(string text)
        {
            var p = new IndexPattern();
            foreach (var raw in text.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries))
            {
                var part = MessageCatalog.AsciiLower(raw.Trim());
                if (part is "*" or "all") { p._all = true; continue; }
                if (part == "last") { p._last = true; continue; }
                int dash = part.IndexOf('-');
                if (dash >= 0)
                {
                    if (!Robot.Int(part.Substring(0, dash), out var l)) return null;
                    var hi = part.Substring(dash + 1);
                    if (hi.Length == 0) { p._ranges.Add((l, int.MaxValue)); continue; }
                    if (!Robot.Int(hi, out var h) || h < l) return null;
                    p._ranges.Add((l, h));
                }
                else
                {
                    if (!Robot.Int(part, out var n) || n < 0) return null;
                    p._ranges.Add((n, n));
                }
            }
            return p;
        }

        public bool Matches(int value, int maxValue) =>
            _all || (_last && value == maxValue) || _ranges.Any(r => value >= r.Low && value <= r.High);
    }
}
