/// The command line of a command step.
public enum CommandArgs {
    /// The arguments are split first and filled in afterwards, so a value with spaces stays one argument, and a lone
    /// `{files}` becomes one argument per file. With an interpreter, the program is its first argument. A leading ~
    /// is `home`. (Choosing an interpreter by extension when none is set is the process launcher's: it depends on
    /// the system and the file's mode.)
    public static func build(_ step: StepDefinition, values: [String: String], files: [String], home: String?) -> CommandLine {
        let c = step.command
        var args: [String] = []
        for token in ArgumentSplitter.split(c.arguments) {
            if token == "{files}" { args += files } else { args.append(TemplateEngine.render(token, values: values)) }
        }
        let exe = expandHome(TemplateEngine.render(c.executable, values: values), home)
        let interpreter = expandHome(c.interpreter, home)
        return interpreter.isEmpty ? CommandLine(executable: exe, arguments: args) : CommandLine(executable: interpreter, arguments: [exe] + args)
    }

    /// `~` and `~/…` (or `~\…`) start in `home`; anything else, or no home, stays as it is.
    static func expandHome(_ path: String, _ home: String?) -> String {
        guard let home, path.hasPrefix("~") else { return path }
        if path == "~" { return home }
        let rest = path.dropFirst()
        guard rest.first == "/" || rest.first == "\\" else { return path }
        var h = Substring(home)
        while h.last == "/" || h.last == "\\" { h = h.dropLast() }
        return String(h) + rest
    }
}
