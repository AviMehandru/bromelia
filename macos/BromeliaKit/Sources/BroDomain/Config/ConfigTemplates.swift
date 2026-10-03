import BroFoundation

/// Ready-made profiles.
public enum ConfigTemplates {
    /// Archive everything (today's preset, now a profile any drive can use): a decrypted backup then MKV of every
    /// title, keeping the backup and the unsplit play-all title, with every archive file. Without an id: the caller
    /// gives it one when adding it.
    public static func archiveEverything() -> Profile {
        Profile(JsonValue.parse("""
            {"name": "Archive everything",
             "mode": {"default": "backupThenMkv"},
             "titles": {"strategy": "all"},
             "makemkv": {"profile": {"mode": "generated", "generated": {"selectionRule": "+sel:all"}}},
             "backup": {"format": "folder", "keepAfterMkv": true},
             "archive": {"checksums": true, "archiveRecord": true, "verifyRips": true, "writeDiscInfo": true},
             "episodes": {"keepPlayAll": true}}
            """)!)
    }
}
