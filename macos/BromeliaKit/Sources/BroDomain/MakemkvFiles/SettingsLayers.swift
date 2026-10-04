/// The settings.conf a job's makemkvcon reads.
public enum SettingsLayers {
    /// `global` (makemkv.settings), then the profile's, then the drive's; empty values are dropped. app_Key is the
    /// registration key when there is one; app_DefaultSelectionString the selection override (hand-picked tracks:
    /// +sel:all) when there is one; app_DataDir, unless a layer sets it, the user's real MakeMKV data folder (the job
    /// runs with its own home).
    public static func merge(_ global: [String: String], profile: [String: String], drive: [String: String], registrationKey: String?,
                             selectionOverride: String?, dataDir: String?) -> [String: String] {
        var o = global
        for (k, v) in profile { o[k] = v }
        for (k, v) in drive { o[k] = v }
        if let key = registrationKey, !key.isEmpty { o["app_Key"] = key }
        if let selectionOverride { o["app_DefaultSelectionString"] = selectionOverride }
        if let dataDir, !dataDir.isEmpty, (o["app_DataDir"] ?? "").isEmpty { o["app_DataDir"] = dataDir }
        return o.filter { !$0.value.isEmpty }
    }
}
