using System.Collections.Generic;
using System.Linq;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// MakeMKV as the engine sets it up (plan §11.1), as far as phase 3 needs it: what each makemkvcon run gets
/// (<see cref="IMakemkvSetup"/>). The settings are SettingsLayers.merge of the configuration's makemkv.settings and the
/// profile's (the drive's own settings are already in it), with the registration key (makemkv.registrationKey through
/// ConfigService.secret), else the key MakeMKV is registered with (app_Key in the user's settings.conf, on macOS and
/// Linux, where a run reads only its own; the registry keeps it on Windows), and the user's MakeMKV data folder. The
/// profile file is the generated one (ProfileXml.render) or the custom file, the switches come from the profile's
/// makemkv, and the run lives in the job's home folder with its transcript in the job's makemkv.txt. Version,
/// registration state and the beta key are phase 5. Every call is safe from any thread; it reads the user's
/// settings.conf, so not on the executor. shared/fixtures/engine/makemkv-service.cases.json.
/// </summary>
public sealed class MakemkvService : IMakemkvSetup
{
    readonly IFileSystem _fs;
    readonly ConfigService _config;
    readonly string _dataDir;
    readonly string? _userSettings;
    readonly string _home;

    /// <param name="dataDir">The user's MakeMKV data folder, for app_DataDir ("" on Windows: not set).</param>
    /// <param name="userSettings">The user's settings.conf, where MakeMKV keeps its key (null on Windows).</param>
    /// <param name="home">The user's home folder, for a leading ~ in a custom profile path or app_DataDir.</param>
    public MakemkvService(IFileSystem fs, ConfigService config, string dataDir, string? userSettings, string home)
    {
        _fs = fs;
        _config = config;
        _dataDir = dataDir;
        _userSettings = userSettings;
        _home = home;
    }

    /// <summary>What one run of <paramref name="profile"/> gets, from the job's configuration; a failure to read the
    /// key (the secret's, or the user's settings.conf) is the call's.</summary>
    public MakemkvInvocation Invocation(Config config, EffectiveProfile profile, string? selectionOverride, string jobDirectory)
    {
        var makemkv = profile.Profile["makemkv"];
        var file = makemkv?["profile"];
        var mode = file?["mode"]?.AsString ?? "makemkvDefault";
        var generated = file?["generated"] ?? JsonValue.Of(System.Array.Empty<(string, JsonValue)>());
        var rule = generated["selectionRule"]?.AsString ?? "";
        string? xml = null;
        string? profilePath = null;
        var selection = selectionOverride;
        if (mode == "generated")
        {
            xml = ProfileXml.Render(selectionOverride == null ? generated : With(generated, "selectionRule", selectionOverride));
            if (selection == null && rule.Trim().Length > 0) selection = rule;
        }
        else if (mode == "customFile" && Expand(file?["customPath"]?.AsString ?? "") is { Length: > 0 } custom)
            profilePath = custom;

        var settings = SettingsLayers.Merge(Strings(config.Document["makemkv"]?["settings"]), Strings(makemkv?["settings"]),
            new Dictionary<string, string>(), Key(config), selection, _dataDir);
        if (settings.TryGetValue("app_DataDir", out var dataDir)) settings["app_DataDir"] = Expand(dataDir);

        var options = new MakemkvOptions(profilePath, (int?)makemkv?["minLengthSeconds"]?.AsInteger, (int?)makemkv?["cacheMB"]?.AsInteger,
            makemkv?["directIO"]?.AsBool, makemkv?["extraArguments"]?.AsString ?? "");
        var stall = config.Document["limits"]?["stallTimeoutMinutes"]?.AsInteger ?? 0;
        return new MakemkvInvocation(new MakemkvRunSettings(settings, xml, _dataDir, jobDirectory + "/home"), options,
            stall > 0 ? new Duration(stall * 60) : null, jobDirectory + "/makemkv.txt");
    }

    /// <summary>makemkv.registrationKey's secret when it has a value, else app_Key in the user's settings.conf, else
    /// none.</summary>
    string? Key(Config config)
    {
        if (config.Document["makemkv"]?["registrationKey"]?["secret"]?.AsString is { } name && _config.Secret(name, config) is { Length: > 0 } key)
            return key;
        if (_userSettings == null || !_fs.Exists(_userSettings)) return null;
        return SettingsConf.Parse(Encoding.UTF8.GetString(_fs.Read(_userSettings))).TryGetValue("app_Key", out var installed) && installed.Length > 0
            ? installed
            : null;
    }

    /// <summary>~ and ~/… under the user's home; anything else as it is.</summary>
    string Expand(string path) =>
        path == "~" ? _home : path.StartsWith("~/", System.StringComparison.Ordinal) ? _home + path.Substring(1) : path;

    static Dictionary<string, string> Strings(JsonValue? map) =>
        (map?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => m.Key, m => m.Value.AsString ?? "");

    static JsonValue With(JsonValue obj, string key, string value) =>
        JsonValue.Of(obj.AsObject!.Where(m => m.Key != key).Select(m => (m.Key, m.Value)).Append((key, JsonValue.Of(value))).ToArray());
}
