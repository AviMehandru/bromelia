using System.Collections.Generic;
using System.Text;
using Bromelia.Domain;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon run gets HOME set to the job's home
/// folder, holding its own settings.conf (0600: it may hold the registration key) and the generated profile. The
/// files stay with the job.</summary>
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
        return new Lease(home, profile);
    }

    sealed class Lease : IIsolationLease
    {
        readonly string _home;
        readonly string? _profile;

        public Lease(string home, string? profile)
        {
            _home = home;
            _profile = profile;
        }

        public IReadOnlyDictionary<string, string> Environment() => new Dictionary<string, string> { ["HOME"] = _home };

        public string? ProfilePath() => _profile;

        public void FirstOutput() { }

        public void Release() { }
    }
}
