using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class NamingTests
{
    private static Dictionary<string, string> ValuesOf(JsonValue? v) =>
        (v?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => m.Key, m => m.Value.AsString!);

    [Fact]
    public void NamingCases() => RunCases("domain/naming.cases.json", (id, given, expect) =>
    {
        if (given["template"] is { } template)
        {
            var values = ValuesOf(given["values"]);
            if (expect["render"] is { } r) Same(r.AsString, TemplateEngine.Render(template.AsString!, values), "render");
            else Same(expect["renderPath"]!.AsString, TemplateEngine.RenderPath(template.AsString!, values), "renderPath");
            return true;
        }
        if (given["component"] is { } component)
        {
            Same(expect["sanitize"]!.AsString, Sanitizer.Component(component.AsString!), "sanitize");
            return true;
        }
        if (given["layout"] is { } layout)
        {
            var naming = new NamingSettings(EnumWire.Parse<Layout>(layout.AsString)!.Value);
            var values = ValuesOf(given["values"]);
            values["kind"] = given["kind"]!.AsString!;
            var folder = Layouts.Folder(naming, values);
            var empty = new Dictionary<string, string>();
            var outputs = new List<(string Key, PlannedOutput Output)>
            {
                ("episodePath", new PlannedOutput(PathRole.Episode, empty)),
                ("mainFeature", new PlannedOutput(PathRole.Title, empty, MainFeature: true)),
                ("extra", new PlannedOutput(PathRole.Title, empty)),
                ("backup", new PlannedOutput(PathRole.Backup, empty)),
            };
            var paths = Layouts.Paths(naming, values, outputs.Select(o => o.Output).ToList());
            if (expect["folder"] is { } f) Same(f.AsString, folder, "folder");
            for (int i = 0; i < outputs.Count; i++)
                if (expect[outputs[i].Key] is { } want) Same(folder + "/" + want.AsString, paths[i].Path, outputs[i].Key);
            return true;
        }
        if (given["title"] is { } title)
        {
            Same(expect["trackLabel"]!.AsString, TokenRegistry.TrackLabel(TitlesTests.TitleOf(title)), "track label");
            return true;
        }
        if (given["episode"] is { } episode)
        {
            Same(expect["episodeLabel"]!.AsString, TokenRegistry.EpisodeLabel((int)episode.AsInteger!, (int)given["width"]!.AsInteger!), "episode label");
            return true;
        }
        if (given["name"] is { } name)
        {
            var existing = given["existing"]!.AsArray!.Select(e => e.AsString!).ToList();
            Same(expect["next"]!.AsString, ConflictNamer.Next(name.AsString!, existing, isFolder: !name.AsString!.Contains('.')), "next");
            return true;
        }
        return false;
    });

    [Fact]
    public void ArgumentCases() => RunCases("domain/arguments.cases.json", id => id.StartsWith("split") || id.StartsWith("quote"), (id, given, expect) =>
    {
        if (given["text"] is { } text) Same(expect["split"], JsonValue.Of(ArgumentSplitter.Split(text.AsString!).Select(JsonValue.Of)), "split");
        else Same(expect["quoted"]!.AsString, ArgumentSplitter.Quote(given["quote"]!.AsString!), "quoted");
        return true;
    });

    [Fact]
    public void TokensAndTemplateLayout()
    {
        var identity = Identity.Resolve(new IdentityInputs(null, "ONE_PIECE_S2_P7_D2", false, Format: DiscFormat.Dvd));
        var values = TokenRegistry.Values(identity, new TokenContext("Rip", "Left", "ONE_PIECE_S2_P7_D2", "ONE_PIECE_S2_P7_D2", DiscType.Dvd, "3f2504e0",
            null, new LocalTime(2026, 10, 3, 9, 5, 7)));
        Assert.Equal("One Piece", values["name"]);
        Assert.Equal("Season 2 Part 7 Disc 2", values["discLabel"]);
        Assert.Equal("2026-10-03", values["date"]);
        Assert.Equal("09-05-07", values["time"]);
        Assert.Equal("TV Shows", values["libraryFolder"]);
        var naming = new NamingSettings();
        var outputs = new List<PlannedOutput>
        {
            new(PathRole.Episode, new Dictionary<string, string> { ["episode"] = "Episode 138", ["track"] = "Title 11 Ch 1-7" }, Title: 0, Episode: 138, Extension: ".mkv"),
            new(PathRole.Backup, new Dictionary<string, string>(), Extension: ""),
        };
        var paths = Layouts.Paths(naming, values, outputs);
        Assert.Equal("One Piece - Season 2 Part 7 Disc 2/One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD.mkv", paths[0].Path);
        Assert.Equal(138, paths[0].Episode);
        Assert.Equal("One Piece - Season 2 Part 7 Disc 2/backup", paths[1].Path);
    }
}
