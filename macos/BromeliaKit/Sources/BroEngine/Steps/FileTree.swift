import BroFoundation
import BroPorts

/// What is under a folder a job made (its staging, a backup in it), through the FileSystem port.
public enum FileTree {
    /// Every file under `root` as a path relative to it ('/' between names), in ordinal order; an empty folder is its path
    /// with a trailing '/'.
    public static func files(_ fs: any FileSystem, root: String) throws(BroError) -> [String] {
        var files: [String] = []
        func walk(_ folder: String, _ prefix: String) throws(BroError) {
            let entries = try fs.list(folder)
            if entries.isEmpty && !prefix.isEmpty { files.append(prefix) }
            for e in entries {
                if e.isDirectory { try walk(folder + "/" + e.name, prefix + e.name + "/") } else { files.append(prefix + e.name) }
            }
        }
        try walk(root, "")
        return files.sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
    }

    /// Removes a file, or a folder with everything in it (files first, then the folders, deepest first).
    public static func remove(_ fs: any FileSystem, path: String) throws(BroError) {
        if try fs.stat(path).isDirectory {
            for e in try fs.list(path).sorted(by: { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }) {
                try remove(fs, path: path + "/" + e.name)
            }
        }
        try fs.remove(path)
    }

    /// Syncs every file under `path` (or the file itself) to disk.
    public static func sync(_ fs: any FileSystem, path: String) throws(BroError) {
        guard try fs.stat(path).isDirectory else {
            try fs.syncFile(path)
            return
        }
        for file in try files(fs, root: path) where !file.hasSuffix("/") { try fs.syncFile(path + "/" + file) }
    }
}
