// Standalone, dependency-free tests for the terminal-launch decision logic.
// Run via `make test`.
#include "../terminal_launch.h"

#include <iostream>
#include <set>

namespace {

int g_failures = 0;

void expect_eq(const std::string& label, const std::string& actual,
                const std::string& expected) {
    if (actual == expected) {
        std::cout << "PASS: " << label << "\n";
        return;
    }
    ++g_failures;
    std::cout << "FAIL: " << label << "\n  expected: [" << expected
              << "]\n  actual:   [" << actual << "]\n";
}

void expect_true(const std::string& label, bool cond) {
    std::cout << (cond ? "PASS: " : "FAIL: ") << label << "\n";
    if (!cond) ++g_failures;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += " | ";
        out += v[i];
    }
    return out;
}

// An is_available that answers yes only for the named binaries.
std::function<bool(const std::string&)> only(const std::set<std::string>& present) {
    return [present](const std::string& b) { return present.count(b) > 0; };
}

} // namespace

int main() {
    // --- command_program ---
    expect_eq("plain command", command_program("vim"), "vim");
    expect_eq("command with arguments", command_program("vim file.txt"), "vim");
    expect_eq("leading whitespace", command_program("   htop  "), "htop");
    expect_eq("absolute path is reduced to the program",
              command_program("/usr/bin/vim f"), "vim");
    expect_eq("relative path is reduced too", command_program("./bin/htop"), "htop");
    expect_eq("empty line", command_program(""), "");
    expect_eq("whitespace only", command_program("   "), "");

    // sudo and its options must be stepped over
    expect_eq("sudo prefix", command_program("sudo vim /etc/hosts"), "vim");
    expect_eq("sudo with a flag", command_program("sudo -E vim f"), "vim");
    expect_eq("sudo -u takes an argument", command_program("sudo -u bob vim f"), "vim");
    expect_eq("sudo -u with a path", command_program("sudo -u bob /usr/bin/htop"), "htop");
    expect_eq("sudo on its own", command_program("sudo"), "");
    expect_eq("sudo with only flags", command_program("sudo -E"), "");

    // leading environment assignments
    expect_eq("one env assignment", command_program("EDITOR=vi vim f"), "vim");
    expect_eq("several env assignments",
              command_program("A=1 B=2 htop"), "htop");
    expect_eq("env assignment then sudo",
              command_program("A=1 sudo vim f"), "vim");

    // A multi-line SELECTION is a supported way to run commands, so the
    // newline must separate words like any other whitespace - otherwise the
    // whole block reads as one program name and nothing is ever detected.
    expect_eq("a newline separates words", command_program("htop\nls"), "htop");
    expect_eq("...with arguments on the first line",
              command_program("vim f\nmake"), "vim");
    expect_eq("a carriage return separates too", command_program("htop\r\nls"), "htop");

    // sudo's short options can be bundled, and can carry their value attached.
    expect_eq("bundled short options ending in one that takes a value",
              command_program("sudo -Hu bob vim f"), "vim");
    expect_eq("a short option with its value attached",
              command_program("sudo -ubob vim f"), "vim");
    expect_eq("another bundle", command_program("sudo -iu bob htop"), "htop");
    expect_eq("a long option with an attached value",
              command_program("sudo --user=bob vim f"), "vim");
    expect_eq("a long option with a separate value",
              command_program("sudo --user bob vim f"), "vim");

    // --- sudo_options_offset: where a `-A` flag has to be inserted ---
    auto insert_A = [](const std::string& cmd) {
        size_t at = sudo_options_offset(cmd);
        if (at == std::string::npos) return std::string("(not sudo)");
        return cmd.substr(0, at) + " -A" + cmd.substr(at);
    };
    expect_eq("plain sudo", insert_A("sudo apt update"), "sudo -A apt update");
    expect_eq("sudo after an env assignment",
              insert_A("FOO=bar sudo apt update"), "FOO=bar sudo -A apt update");
    expect_eq("sudo after several assignments",
              insert_A("A=1 B=2 sudo x"), "A=1 B=2 sudo -A x");
    expect_eq("sudo by absolute path",
              insert_A("/usr/bin/sudo vim f"), "/usr/bin/sudo -A vim f");
    expect_eq("sudo with leading whitespace", insert_A("  sudo ls"), "  sudo -A ls");
    expect_eq("sudo on its own", insert_A("sudo"), "sudo -A");
    expect_eq("not sudo at all", insert_A("apt update"), "(not sudo)");
    expect_eq("a program merely starting with those letters",
              insert_A("sudoedit f"), "(not sudo)");
    expect_eq("sudo not in first position is not ours to rewrite",
              insert_A("time sudo x"), "(not sudo)");

    // --- needs_terminal: the programs that must go to a terminal ---
    for (const char* p : {"vi", "vim", "vimdiff", "nvim", "view", "nano", "pico",
                           "emacs", "joe", "jed", "micro", "helix", "hx", "kak",
                           "mc", "ranger", "vifm", "nnn", "lf",
                           "top", "htop", "btop", "atop", "watch", "glances",
                           "iotop", "iftop", "nvtop", "nload", "bmon", "ncdu"}) {
        expect_true(std::string("needs a terminal: ") + p, needs_terminal(p));
    }

    // with arguments, paths and sudo, still detected
    expect_true("vim with a file", needs_terminal("vim /etc/hosts"));
    expect_true("sudo vim", needs_terminal("sudo vim /etc/hosts"));
    expect_true("watch with flags", needs_terminal("watch -n 1 ls -la"));
    expect_true("absolute path to htop", needs_terminal("/usr/bin/htop"));

    // --- needs_terminal: the line-oriented commands that must NOT ---
    for (const char* p : {"ping localhost", "ping -c 4 8.8.8.8", "ls -la", "echo hi",
                           "grep foo bar.txt", "sudo apt upgrade", "make", "tail -f log",
                           "python3", "cat file", "sleep 30", "git status"}) {
        expect_true(std::string("stays in the buffer: ") + p, !needs_terminal(p));
    }
    expect_true("an empty line needs no terminal", !needs_terminal(""));

    // Exact match only - a longer name that merely starts with a listed one
    // must not be caught.
    expect_true("'vimeo' is not 'vim'", !needs_terminal("vimeo upload"));
    expect_true("'topple' is not 'top'", !needs_terminal("topple"));
    expect_true("'watchman' is not 'watch'", !needs_terminal("watchman status"));
    expect_true("'nanorc' is not 'nano'", !needs_terminal("nanorc"));

    // Only the first program is examined - documented limitation.
    expect_true("a compound line is not inspected past the first command",
                !needs_terminal("cd /tmp && vim x"));

    // --- find_terminal_emulator ---
    TerminalEmulator term;

    expect_true("finds ptyxis when it is the only one installed",
                find_terminal_emulator("", only({"ptyxis"}), term));
    expect_eq("...and names it", term.binary, "ptyxis");
    expect_eq("...with the arguments ptyxis needs", join(term.pre_args),
              "--new-window | --");

    expect_true("finds xterm when it is the only one",
                find_terminal_emulator("", only({"xterm"}), term));
    expect_eq("...as xterm", term.binary, "xterm");
    expect_eq("...with -e", join(term.pre_args), "-e");

    expect_true("kitty needs no preceding arguments",
                find_terminal_emulator("", only({"kitty"}), term));
    expect_eq("...kitty", term.binary, "kitty");
    expect_true("...and gets none", term.pre_args.empty());

    // Priority: a better-known emulator wins over a fallback.
    expect_true("picks between several", find_terminal_emulator(
                    "", only({"xterm", "gnome-terminal", "ptyxis"}), term));
    expect_eq("prefers ptyxis over gnome-terminal and xterm", term.binary, "ptyxis");

    // $TERMINAL wins when it is available...
    expect_true("honours $TERMINAL",
                find_terminal_emulator("konsole", only({"konsole", "ptyxis"}), term));
    expect_eq("...over the priority list", term.binary, "konsole");
    expect_eq("...with konsole's own flag", join(term.pre_args), "-e");

    // ...but a $TERMINAL that isn't installed falls back.
    expect_true("an uninstalled $TERMINAL falls back to the list",
                find_terminal_emulator("konsole", only({"xterm"}), term));
    expect_eq("...to what is installed", term.binary, "xterm");

    // A $TERMINAL given as a path must still be recognised, and still be
    // exec'd by the exact path the user gave.
    expect_true("an absolute $TERMINAL path is recognised",
                find_terminal_emulator("/usr/bin/gnome-terminal",
                                        only({"/usr/bin/gnome-terminal"}), term));
    expect_eq("...and exec'd by that exact path", term.binary, "/usr/bin/gnome-terminal");
    expect_eq("...with the arguments its basename calls for", join(term.pre_args), "--");

    // An emulator nobody listed still has to be given SOME exec flag, or it
    // just opens an empty shell and silently drops the command.
    expect_true("an unlisted $TERMINAL is used",
                find_terminal_emulator("lxterminal", only({"lxterminal"}), term));
    expect_eq("...by name", term.binary, "lxterminal");
    expect_eq("...with -e, the near-universal convention", join(term.pre_args), "-e");

    expect_true("reports failure when nothing is installed",
                !find_terminal_emulator("", only({}), term));

    // --- build_terminal_argv ---
    TerminalEmulator ptyxis{"ptyxis", {"--new-window", "--"}};
    expect_eq("argv for ptyxis", join(build_terminal_argv(ptyxis, "htop")),
              "ptyxis | --new-window | -- | bash | -c | htop");

    TerminalEmulator kitty{"kitty", {}};
    expect_eq("argv for an emulator with no pre-args",
              join(build_terminal_argv(kitty, "vim f")),
              "kitty | bash | -c | vim f");

    // The command is one argv entry, so quoting inside it is untouched.
    expect_eq("the command stays a single argument, quotes and all",
              join(build_terminal_argv(kitty, "watch -n 1 'ls | wc -l'")),
              "kitty | bash | -c | watch -n 1 'ls | wc -l'");

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All terminal launch tests passed\n";
    return 0;
}
