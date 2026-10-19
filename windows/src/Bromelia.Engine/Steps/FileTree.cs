using System;
using System.Collections.Generic;
using System.Linq;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>What is under a folder a job made (its staging, a backup in it), through the FileSystem port.</summary>
public static class FileTree
{
    /// <summary>Every file under <paramref name="root"/> as a path relative to it ('/' between names), in ordinal order;
    /// an empty folder is its path with a trailing '/'.</summary>
    public static IReadOnlyList<string> Files(IFileSystem fs, string root)
    {
        var files = new List<string>();
        void Walk(string folder, string prefix)
        {
            var entries = fs.List(folder);
            if (entries.Count == 0 && prefix.Length > 0) files.Add(prefix);
            foreach (var e in entries)
            {
                if (e.IsDirectory) Walk(folder + "/" + e.Name, prefix + e.Name + "/");
                else files.Add(prefix + e.Name);
            }
        }
        Walk(root, "");
        files.Sort(StringComparer.Ordinal);
        return files;
    }

    /// <summary>Removes a file, or a folder with everything in it (files first, then the folders, deepest first).</summary>
    public static void Remove(IFileSystem fs, string path)
    {
        if (fs.Stat(path).IsDirectory)
            foreach (var e in fs.List(path).OrderBy(e => e.Name, StringComparer.Ordinal)) Remove(fs, path + "/" + e.Name);
        fs.Remove(path);
    }

    /// <summary>Syncs every file under <paramref name="path"/> (or the file itself) to disk.</summary>
    public static void Sync(IFileSystem fs, string path)
    {
        if (!fs.Stat(path).IsDirectory)
        {
            fs.SyncFile(path);
            return;
        }
        foreach (var file in Files(fs, path).Where(f => !f.EndsWith('/'))) fs.SyncFile(path + "/" + file);
    }
}
