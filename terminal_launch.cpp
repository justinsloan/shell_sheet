#include "terminal_launch.h"

#include <set>

namespace {

std::string trim(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = s.find_first_not_of(" \t", i);
        if (start == std::string::npos) break;
        size_t end = s.find_first_of(" \t", start);
        if (end == std::string::npos) end = s.size();
        words.push_back(s.substr(start, end - start));
        i = end;
    }
    return words;
}

// `VAR=value` - a leading assignment rather than the command itself. The '='
// must come before any '/', so a path like ./a=b/prog isn't mistaken for one.
bool is_env_assignment(const std::string& word) {
    size_t eq = word.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    size_t slash = word.find('/');
    return slash == std::string::npos || eq < slash;
}

// sudo options that consume the following word, so it isn't mistaken for the
// program being run.
bool sudo_option_takes_argument(const std::string& option) {
    static const std::set<std::string> with_argument = {
        "-u", "--user", "-g", "--group", "-p", "--prompt",
        "-C", "--close-from", "-h", "--host", "-R", "--chroot",
        "-D", "--chdir", "-t", "--type", "-r", "--role", "-U", "--other-user",
    };
    return with_argument.count(option) > 0;
}

std::string strip_directory(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Full-screen programs: they drive the whole terminal, so there is nothing
// sensible for this app to do with them in a text buffer. Kept deliberately
// short and specific - anything not here still runs in the buffer, and can
// be forced into a terminal from the Terminal menu.
const std::set<std::string>& full_screen_programs() {
    static const std::set<std::string> programs = {
        // Editors and file managers
        "vi", "vim", "vimdiff", "nvim", "view", "nano", "pico", "emacs",
        "joe", "jed", "micro", "helix", "hx", "kak",
        "mc", "ranger", "vifm", "nnn", "lf",
        // Monitors and watchers
        "top", "htop", "btop", "atop", "watch", "glances",
        "iotop", "iftop", "nvtop", "nload", "bmon", "ncdu",
    };
    return programs;
}

// Terminal emulators in preference order, with the arguments that must come
// before the command. Every one of these accepts the command as ordinary
// argv entries; none needs a quoted string, so nothing here has to be
// shell-escaped.
//
// ptyxis is first because it is what `x-terminal-emulator` resolves to on
// current Ubuntu - and note it does NOT accept the traditional `-e`, only
// `--` or `-x`, which is why this table exists instead of one generic call.
const std::vector<TerminalEmulator>& known_emulators() {
    static const std::vector<TerminalEmulator> emulators = {
        {"ptyxis",         {"--new-window", "--"}},
        {"gnome-terminal", {"--"}},
        {"konsole",        {"-e"}},
        {"xfce4-terminal", {"-x"}},
        {"mate-terminal",  {"--"}},
        {"alacritty",      {"-e"}},
        {"kitty",          {}},
        {"foot",           {}},
        {"wezterm",        {"start", "--"}},
        {"terminator",     {"-x"}},
        {"urxvt",          {"-e"}},
        {"st",             {"-e"}},
        {"xterm",          {"-e"}},
    };
    return emulators;
}

} // namespace

std::string command_program(const std::string& command) {
    std::vector<std::string> words = split_words(trim(command));

    size_t i = 0;
    while (i < words.size() && is_env_assignment(words[i])) {
        ++i;
    }

    if (i < words.size() && strip_directory(words[i]) == "sudo") {
        ++i;
        // Step over sudo's own options, and the arguments some of them take.
        while (i < words.size() && !words[i].empty() && words[i][0] == '-') {
            bool takes_argument = sudo_option_takes_argument(words[i]);
            ++i;
            if (takes_argument && i < words.size()) ++i;
        }
        // `sudo VAR=value cmd` is legal too.
        while (i < words.size() && is_env_assignment(words[i])) {
            ++i;
        }
    }

    if (i >= words.size()) return "";
    return strip_directory(words[i]);
}

bool needs_terminal(const std::string& command) {
    std::string program = command_program(command);
    if (program.empty()) return false;
    return full_screen_programs().count(program) > 0;
}

bool find_terminal_emulator(const std::string& terminal_env,
                             const std::function<bool(const std::string&)>& is_available,
                             TerminalEmulator& out) {
    if (!terminal_env.empty() && is_available(terminal_env)) {
        // A $TERMINAL we happen to know gets its proper arguments; one we
        // don't is still honoured, just invoked bare.
        for (const TerminalEmulator& known : known_emulators()) {
            if (known.binary == terminal_env) {
                out = known;
                return true;
            }
        }
        out = TerminalEmulator{terminal_env, {}};
        return true;
    }

    for (const TerminalEmulator& known : known_emulators()) {
        if (is_available(known.binary)) {
            out = known;
            return true;
        }
    }
    return false;
}

std::vector<std::string> build_terminal_argv(const TerminalEmulator& terminal,
                                              const std::string& shell_command) {
    std::vector<std::string> argv;
    argv.push_back(terminal.binary);
    for (const std::string& arg : terminal.pre_args) {
        argv.push_back(arg);
    }
    // bash -c keeps the user's line intact: pipes, quoting and all stay one
    // argv entry, interpreted by the shell inside the terminal.
    argv.push_back("bash");
    argv.push_back("-c");
    argv.push_back(shell_command);
    return argv;
}
