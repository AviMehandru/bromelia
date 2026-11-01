using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Bromelia.Tests;

namespace Bromelia.Scenarios.Tests;

/// <summary>
/// The world a scenario starts in (decision 235), on the test root: <c>{root}</c> is <see cref="TestCompositionRoot.Root"/>
/// and paths use '/' everywhere. The configuration (given.config, else config3-scenario-base.json, then configPatch, then
/// profile over the default profile, <c>{root}</c> filled in) is written to the data folder before the engine starts; each
/// library gets its folder and marker; given.files are written; given.disc is what the ISO (and a drive given no disc)
/// holds, and <see cref="Iso"/> exists as a small file. The drive world (decision 237): each fake drive's device,
/// identification, MakeMKV index, tray and disc, which the drive scan's DRV lines report (no disc while the tray is open);
/// the engine's eject opens the tray, leaving the disc in it (the OS reports TrayOpened), and its close shuts it unless the
/// drive has no tray motor. The tools are the scripts (given.tools; makemkvcon's default is tools/makemkvcon-saves.yaml).
/// </summary>
public sealed class ScenarioWorld
{
    public const string Iso = TestCompositionRoot.Root + "/test.iso";
    const string DefaultConfig = "config3-scenario-base.json";
    const string DefaultMakemkvcon = "tools/makemkvcon-saves.yaml";

    /// <summary>A fake drive: its scenario name, device, identification, MakeMKV index, tray and disc.</summary>
    public sealed class Drive
    {
        public string Name = "";
        public string Device = "";
        public string Identification = "";
        public int Index;
        public bool TrayOpen;
        public bool TrayMotor = true;
        public JsonValue? Disc;
    }

    readonly object _lock = new();
    public readonly JsonValue Given;
    public readonly List<Drive> Drives = new();
    readonly Dictionary<string, JsonValue> _scripts = new();
    /// <summary>The marker written in each library's folder, by library id.</summary>
    readonly Dictionary<string, LibraryMarker> _markers = new();
    JsonValue? _disc;

    public ScenarioWorld(JsonValue scenario)
    {
        Given = scenario["given"] ?? JsonValue.Of();
        _disc = Given["disc"] is { } disc ? ScenarioCatalog.Disc(disc) : null;
        _scripts["makemkvcon"] = ScenarioCatalog.Read(DefaultMakemkvcon);
        foreach (var (tool, script) in Given["tools"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
        {
            if (script.IsNull) _scripts.Remove(tool);
            else _scripts[tool] = ScenarioCatalog.Script(script);
        }
        int order = 0;
        foreach (var d in Given["drives"]?.AsArray ?? new List<JsonValue>())
        {
            Drives.Add(new Drive
            {
                Name = d["id"]!.AsString!,
                Device = d["device"]?.AsString ?? "/dev/" + d["id"]!.AsString,
                Identification = d["identification"]?.AsString ?? d["name"]?.AsString ?? "BD-RE " + d["id"]!.AsString,
                Index = (int)(d["makemkvIndex"]?.AsInteger ?? order),
                TrayMotor = d["trayMotor"]?.AsBool ?? true,
                Disc = d["disc"] is { } driveDisc ? ScenarioCatalog.Disc(driveDisc) : null,
            });
            order++;
        }
    }

    /// <summary>Whether the tool has a script (the locator finds exactly those).</summary>
    public bool Installed(string tool)
    {
        lock (_lock) return _scripts.ContainsKey(tool);
    }

    /// <summary>The tool's script from now on (null: not installed).</summary>
    public void SetScript(string tool, JsonValue? script)
    {
        lock (_lock)
        {
            if (script is null || script.IsNull) _scripts.Remove(tool);
            else _scripts[tool] = ScenarioCatalog.Script(script);
        }
    }

    public JsonValue? Script(string tool)
    {
        lock (_lock) return _scripts.GetValueOrDefault(tool);
    }

    /// <summary>What the ISO (and a folder or file source) holds.</summary>
    public JsonValue? Disc
    {
        get { lock (_lock) return _disc; }
        set { lock (_lock) _disc = value; }
    }

    public Drive DriveNamed(string name) => Drives.FirstOrDefault(d => d.Name == name) ?? throw new ScenarioFailure("a drive " + name, "none");

    // ---- the root ----

    /// <summary>Writes the configuration, libraries, files and the ISO; adds the drives and hooks the tray calls.</summary>
    public void Prepare(TestCompositionRoot root)
    {
        var fs = root.Fs;
        var config = Configuration();
        fs.Put(TestCompositionRoot.ConfigPath, JsonValue.EncodeCanonical(config));
        var markers = (Given["libraries"]?.AsArray ?? new List<JsonValue>()).ToDictionary(l => l["id"]!.AsString!, l => l["marker"]?.AsBool ?? true);
        foreach (var library in config["libraries"]?.AsArray ?? new List<JsonValue>())
        {
            var id = library["id"]!.AsString!;
            var path = library["path"]!.AsString!;
            fs.PutFolder(path);
            if (markers.GetValueOrDefault(id, true))
            {
                var marker = LibraryMarker.Make(id, path, library["name"]?.AsString ?? id, root.Clock.Now());
                fs.Put(path + "/" + LibraryMarker.FileName, LibraryMarker.Encode(marker));
                _markers[id] = marker;
            }
        }
        foreach (var (path, content) in Given["files"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            fs.Put(TestCompositionRoot.Root + "/" + path, FileBytes(content));
        if (Given["freeBytes"]?.AsInteger is { } free) fs.AnyVolume = fs.AnyVolume! with { FreeBytes = free };
        int n = 0;
        foreach (var unit in Given["units"]?.AsArray ?? new List<JsonValue>()) AddUnit(root, config, unit, ++n);
        if (_disc is { } disc) fs.Put(Iso, Encoding.UTF8.GetBytes("an image of " + (disc["volume"]?.AsString ?? "a disc")));
        foreach (var d in Drives)
        {
            root.Drives.Add(new OsDrive(d.Device, d.Identification));
            root.Drives.SetContent(d.Device, Content(d.Disc));
        }
        root.Drives.OnEject = device =>
        {
            Change(device, d => d.TrayOpen = true); // the disc stays in the open tray until a remove
            root.Drives.Send(new DeviceEvent.TrayOpened(device)); // and the OS reports it, as for openTray
        };
        root.Drives.OnCloseTray = device => Change(device, d => d.TrayOpen = !d.TrayMotor);
    }

    /// <summary>An archive unit from before (given.units, README "Catalogue"): its folder with its files ("contents of
    /// &lt;name&gt;, long enough to hash"), SHA256SUMS and a version 3 record bromelia-&lt;unit8&gt;.json, and unless
    /// inCatalogue is false the rows a commit makes (UnitRepository.finishCommit: the committed unit and its files). Its id is
    /// 00000000-0000-4000-8000-2000000000&lt;n&gt;; its library is given.library, else the one whose folder holds it; its
    /// fingerprint is given, or fingerprintOf's (the Domain fingerprint of that disc's generated listing).</summary>
    void AddUnit(TestCompositionRoot root, JsonValue config, JsonValue unit, int n)
    {
        if (unit["record"]?.AsInteger == 2) throw new ScenarioFailure("given.units[].record 2 (phase 5)", "not built");
        var fs = root.Fs;
        var folder = TestCompositionRoot.Root + "/" + unit["folder"]!.AsString!.TrimEnd('/');
        var libraries = config["libraries"]?.AsArray ?? new List<JsonValue>();
        var library = (unit["library"]?.AsString is { } wanted ? libraries.FirstOrDefault(l => l["id"]?.AsString == wanted)
                : libraries.FirstOrDefault(l => folder.StartsWith(l["path"]!.AsString!.TrimEnd('/') + "/", StringComparison.Ordinal)))
            ?? throw new ScenarioFailure("a library holding " + folder, "none");
        var libraryPath = library["path"]!.AsString!.TrimEnd('/');
        var id = new Id($"00000000-0000-4000-8000-2000000000{n:x2}");
        var unit8 = id.Value[..8];
        var fingerprint = unit["fingerprint"]?.AsString
            ?? (unit["fingerprintOf"] is { } of ? Fingerprint.Of(ParsedListing(ScenarioCatalog.Disc(of))) : null);
        var status = unit["status"]?.AsString ?? "success";
        var kind = unit["kind"]?.AsString ?? "movie";
        var name = unit["name"]?.AsString ?? folder[(folder.LastIndexOf('/') + 1)..];
        var at = unit["archivedAt"]?.AsString is { } day ? Instant.Parse(day + "T20:00:00.000Z")!.Value : root.Clock.Now();
        int? Int(string key) => unit[key]?.AsInteger is { } v ? (int)v : null;
        JsonValue Number(string key) => unit[key]?.AsInteger is { } v ? JsonValue.Of(v) : JsonValue.Null.Instance;

        fs.PutFolder(folder);
        var files = new List<UnitFile>();
        foreach (var file in (unit["files"]?.AsArray ?? new List<JsonValue>()).Select(f => f.AsString!))
        {
            var bytes = Encoding.UTF8.GetBytes($"contents of {file}, long enough to hash");
            fs.Put(folder + "/" + file, bytes);
            files.Add(new UnitFile(file, bytes.Length, Sha256Sums.Hash(bytes), file.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase) ? "title" : ""));
        }
        fs.Put(folder + "/" + Sha256Sums.FileName, Encoding.UTF8.GetBytes(Sha256Sums.Render(files.Select(f => new SumEntry(f.Path, f.Sha256)))));
        var recordFile = $"bromelia-{unit8}.json";
        var document = JsonValue.Of(("format", JsonValue.Of("bromelia-archive")), ("version", JsonValue.Of(3)),
            ("unit", JsonValue.Of(("id", JsonValue.Of(id.Value)), ("short", JsonValue.Of(unit8)), ("library", library["id"]!))),
            ("status", JsonValue.Of(status)), ("name", JsonValue.Of(name)), ("kind", JsonValue.Of(kind)),
            ("disc", JsonValue.Of(("label", JsonValue.Of(unit["label"]?.AsString ?? "")), ("fingerprint", JsonValue.Of(fingerprint)),
                ("season", Number("season")), ("part", Number("part")), ("volume", Number("volume")),
                ("disc", Number("disc")))),
            ("episodes", unit["episodes"] ?? JsonValue.Of(Array.Empty<JsonValue>())),
            ("files", JsonValue.Of(files.Select(f => JsonValue.Of(("path", JsonValue.Of(f.Path)), ("size", JsonValue.Of(f.Size)), ("sha256", JsonValue.Of(f.Sha256)))))),
            ("archivedAt", JsonValue.Of(Instant.Format(at))));
        fs.Put(folder + "/" + recordFile, ArchiveRecordCodec.EncodeV3(ArchiveRecordCodec.Decode(JsonValue.EncodeCanonical(document))
            ?? throw new ScenarioFailure("a version 3 record for " + folder, "none")));
        if (unit["inCatalogue"]?.AsBool == false) return;
        var record = new UnitRecord(id, library["id"]!.AsString!, null, null, folder[(libraryPath.Length + 1)..], recordFile, 3, UnitState.Committed,
            EnumWire.Parse<UnitStatus>(status)!.Value, name, EnumWire.Parse<MediaKind>(kind)!.Value, "bluray", "BR", false, fingerprint,
            unit["label"]?.AsString ?? "", Int("season"), Int("part"), Int("volume"), Int("disc"), "", files.Sum(f => f.Size), files.Count, 1, at, at);
        var intent = new CommitIntent(id, id, "", folder, false, false, at, new List<CommitItem>());
        var libraryId = library["id"]!.AsString!;
        // The library's row as attaching it left it (the unit's row needs it), with the marker the folder holds.
        var row = new LibraryRecord(libraryId, library["name"]?.AsString ?? libraryId, libraryPath,
            _markers.TryGetValue(libraryId, out var m) ? m.Id : "none", at, true, at);
        Task.Run(async () =>
        {
            await root.Store.Libraries().Upsert(row);
            await root.Store.Units().FinishCommit(intent, record, files);
        }).GetAwaiter().GetResult();
    }

    /// <summary>The configuration document the engine starts with.</summary>
    public JsonValue Configuration()
    {
        var config = Fixtures.Json("config/" + (Given["config"]?.AsString ?? DefaultConfig));
        if (Given["configPatch"] is { } patch) config = MergePatch(config, patch);
        if (Given["libraries"]?.AsArray is { } libraries)
            config = MergePatch(config, JsonValue.Of(("libraries", JsonValue.Of(libraries.Select(l => JsonValue.Of(
                ("id", l["id"]!), ("name", l["name"] ?? l["id"]!), ("path", JsonValue.Of(Absolute(l["path"]!.AsString!)))))))));
        if (Given["profile"] is { } profile)
        {
            var profiles = (config["profiles"]?.AsArray ?? new List<JsonValue>()).ToList();
            int at = profiles.FindIndex(p => p["id"]?.AsString == "default");
            if (at < 0) profiles.Add(MergePatch(JsonValue.Of(("id", JsonValue.Of("default"))), profile));
            else profiles[at] = MergePatch(profiles[at], profile);
            config = MergePatch(config, JsonValue.Of(("profiles", JsonValue.Of(profiles))));
        }
        return Fill(config);
    }

    /// <summary>A path the scenario gives relative to the root, as an absolute one ({root} stays for <see cref="Fill"/>).</summary>
    static string Absolute(string path) => path.StartsWith('/') || path.StartsWith("{root}", StringComparison.Ordinal) ? path : "{root}/" + path;

    /// <summary>RFC 7386.</summary>
    public static JsonValue MergePatch(JsonValue target, JsonValue patch)
    {
        if (patch is not JsonValue.Object p) return patch;
        var members = (target as JsonValue.Object)?.Members.ToList() ?? new List<KeyValuePair<string, JsonValue>>();
        foreach (var (key, value) in p.Members)
        {
            int at = members.FindIndex(m => m.Key == key);
            if (value.IsNull)
            {
                if (at >= 0) members.RemoveAt(at);
                continue;
            }
            var merged = MergePatch(at >= 0 ? members[at].Value : JsonValue.Null.Instance, value);
            if (at >= 0) members[at] = new(key, merged);
            else members.Add(new(key, merged));
        }
        return new JsonValue.Object(members);
    }

    /// <summary>{root} in every string.</summary>
    public static JsonValue Fill(JsonValue v) => v switch
    {
        JsonValue.String s => JsonValue.Of(s.Value.Replace("{root}", TestCompositionRoot.Root)),
        JsonValue.Array a => JsonValue.Of(a.Items.Select(Fill)),
        JsonValue.Object o => new JsonValue.Object(o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Fill(m.Value))).ToList()),
        _ => v,
    };

    /// <summary>A file's bytes from a scenario: text, or {bytes, seed, descriptor} (that many bytes of a seeded pattern, a
    /// volume descriptor at 32769).</summary>
    public static byte[] FileBytes(JsonValue content)
    {
        if (content.AsString is { } text) return Encoding.UTF8.GetBytes(text);
        var bytes = new byte[content["bytes"]?.AsInteger ?? 0];
        var seed = (int)(content["seed"]?.AsInteger ?? 0);
        for (int i = 0; i < bytes.Length; i++) bytes[i] = (byte)((i * 31 + seed * 7) & 0xFF);
        if (content["descriptor"]?.AsString is { } descriptor && bytes.Length >= 32769 + descriptor.Length)
            Encoding.ASCII.GetBytes(descriptor).CopyTo(bytes, 32769);
        return bytes;
    }

    static DiscContent Content(JsonValue? disc) =>
        disc is null ? DiscContent.Unknown : EnumWire.Parse<DiscContent>(disc["content"]?.AsString ?? "video") ?? DiscContent.Video;

    // ---- the drive world ----

    void Change(string device, Action<Drive> change)
    {
        lock (_lock)
            foreach (var d in Drives.Where(d => d.Device == device))
                change(d);
    }

    /// <summary>A disc goes into the drive (its tray closes) and the OS reports it.</summary>
    public void Insert(TestCompositionRoot root, string name, JsonValue disc)
    {
        var d = DriveNamed(name);
        lock (_lock)
        {
            d.Disc = ScenarioCatalog.Disc(disc);
            d.TrayOpen = false;
        }
        root.Drives.SetContent(d.Device, Content(d.Disc));
        root.Drives.Send(new DeviceEvent.MediaArrived(d.Device));
    }

    public void Remove(TestCompositionRoot root, string name)
    {
        var d = DriveNamed(name);
        lock (_lock) d.Disc = null;
        root.Drives.SetContent(d.Device, DiscContent.Unknown);
        root.Drives.Send(new DeviceEvent.MediaRemoved(d.Device));
    }

    public void OpenTray(TestCompositionRoot root, string name)
    {
        var d = DriveNamed(name);
        lock (_lock) d.TrayOpen = true;
        root.Drives.Send(new DeviceEvent.TrayOpened(d.Device));
    }

    /// <summary>The DRV lines of a drive scan: index, state (2 a disc, 1 an open tray, 0 empty), the disc's flags
    /// (DVD 1, HD DVD 2, Blu-ray 4 + AACS 8), identification, label and device.</summary>
    public IReadOnlyList<string> DriveScan()
    {
        lock (_lock)
            return Drives.OrderBy(d => d.Index).Select(d =>
            {
                var disc = d.TrayOpen ? null : d.Disc;
                int state = disc != null ? 2 : d.TrayOpen ? 1 : 0;
                int flags = disc?["type"]?.AsString switch
                {
                    null => 0,
                    "dvd" => 1,
                    "hddvd" => 2,
                    _ => 12,
                };
                if (disc?["flags"]?.AsInteger is { } given) flags = (int)given;
                var label = disc?["label"]?.AsString ?? disc?["volume"]?.AsString ?? "";
                return $"DRV:{d.Index},{state},999,{flags},\"{d.Identification}\",\"{label}\",\"{d.Device}\"";
            }).ToList();
    }

    /// <summary>The disc a makemkvcon source names: disc:N or dev:&lt;device&gt; a drive's, anything else the ISO's.</summary>
    public JsonValue? DiscOf(string source)
    {
        lock (_lock)
        {
            if (source.StartsWith("disc:", StringComparison.Ordinal) && int.TryParse(source[5..], out var index))
                return Drives.FirstOrDefault(d => d.Index == index) is { TrayOpen: false } drive ? drive.Disc : null;
            if (source.StartsWith("dev:", StringComparison.Ordinal))
                return Drives.FirstOrDefault(d => d.Device == source[4..]) is { TrayOpen: false } drive ? drive.Disc : null;
            return _disc;
        }
    }

    /// <summary>The device a makemkvcon source names ("" for an image).</summary>
    public string DeviceOf(string source)
    {
        lock (_lock)
        {
            if (source.StartsWith("disc:", StringComparison.Ordinal) && int.TryParse(source[5..], out var index))
                return Drives.FirstOrDefault(d => d.Index == index)?.Device ?? "";
            return source.StartsWith("dev:", StringComparison.Ordinal) ? source[4..] : "";
        }
    }

    // ---- listings ----

    /// <summary>A disc's listing (README "A generated listing", or its listing fixture), without the titles shorter than
    /// <paramref name="minLength"/> seconds, which renumbers the rest.</summary>
    public static string Listing(JsonValue disc, int? minLength = null)
    {
        if (disc["listing"]?.AsString is { } fixture) return Fixtures.Text(fixture);
        var titles = (disc["titles"]?.AsArray ?? new List<JsonValue>())
            .Where(t => minLength is not { } m || Seconds(t["duration"]?.AsString ?? "0") >= m).ToList();
        return Fixtures.GeneratedListing(JsonValue.Of(("volume", disc["volume"] ?? JsonValue.Of("")), ("type", disc["type"] ?? JsonValue.Of("bluray")),
            ("titles", JsonValue.Of(titles))));
    }

    /// <summary>The listing as the engine reads it (Robot.parseLine, ListingBuilder).</summary>
    public static Listing ParsedListing(JsonValue disc)
    {
        var builder = new ListingBuilder();
        foreach (var line in Listing(disc).Split('\n', StringSplitOptions.RemoveEmptyEntries))
            if (Robot.ParseLine(line) is { } e) builder.Feed(e);
        return builder.Build();
    }

    /// <summary>h:mm:ss as seconds.</summary>
    public static int Seconds(string duration) => duration.Split(':').Aggregate(0, (total, part) => total * 60 + int.Parse(part));
}
