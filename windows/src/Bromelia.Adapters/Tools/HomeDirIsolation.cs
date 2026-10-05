using System.Collections.Generic;
using System.Linq;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon run gets HOME set to the job's home
/// folder, holding its own settings.conf (0600: it may hold the registration key) and the generated profile. The
/// files stay with the job, except the key: release removes the app_Key line from settings.conf, whatever wrote it, so
/// the key doesn't wait in the job's folder for retention.</summary>
public sealed class HomeDirIsolation : ISettingsIsolation
{
    readonly IFileSystem _fs;
    readonly HomeLayout _layout;

    public HomeDirIsolation(IFileSystem fs, HomeLayout layout)
    {
        _fs = fs;
        _layout = layout;
    }

    public IIsolationLease Prepare(MakemkvRunSettings settings)
    {
        var home = settings.WorkDirectory;
        var folder = home + (_layout == HomeLayout.Macos ? "/Library/MakeMKV" : "/.MakeMKV");
        _fs.CreateDirectory(folder, parentsMustExist: false);
        var conf = SettingsConf.Render(settings.Settings, "Written by Bromelia for one makemkvcon run");
        _fs.WriteAtomically(folder + "/settings.conf", Encoding.UTF8.GetBytes(conf), 0x180);
        string? profile = null;
        if (settings.ProfileXml is { } xml)
        {
            profile = home + "/profile.mmcp.xml";
            _fs.WriteAtomically(profile, Encoding.UTF8.GetBytes(xml), 0x1A4);
        }
        return new Lease(_fs, home, folder + "/settings.conf", profile);
    }

    /// <summary>A settings.conf line that sets app_Key.</summary>
    internal static bool IsKeyLine(string line)
    {
        var t = line.TrimStart();
        return t.StartsWith("app_Key", System.StringComparison.Ordinal) && t.Substring("app_Key".Length).TrimStart().StartsWith('=');
    }

    sealed class Lease : IIsolationLease
    {
        readonly IFileSystem _fs;
        readonly string _home;
        readonly string _conf;
        readonly string? _profile;

        public Lease(IFileSystem fs, string home, string conf, string? profile)
        {
            _fs = fs;
            _home = home;
            _conf = conf;
            _profile = profile;
        }

        public IReadOnlyDictionary<string, string> Environment() => new Dictionary<string, string> { ["HOME"] = _home };

        public string? ProfilePath() => _profile;

        public void FirstOutput() { }

        public void Release()
        {
            try
            {
                if (!_fs.Exists(_conf)) return;
                var text = Encoding.UTF8.GetString(_fs.Read(_conf));
                var lines = text.Split('\n');
                var kept = string.Join("\n", lines.Where(l => !IsKeyLine(l)));
                if (kept != text) _fs.WriteAtomically(_conf, Encoding.UTF8.GetBytes(kept), 0x180);
            }
            catch (BroFailure) { } // best effort: the file is 0600 and pruned with the job
        }
    }
}
