#ifndef TERMINAL_LAUNCH_H
#define TERMINAL_LAUNCH_H

#include <functional>
#include <string>
#include <vector>

// Some programs cannot live in this app's buffer at all. The buffer handles
// LINE-oriented commands, interactive ones included - typed lines are
// forwarded to the child's stdin, so `apt`'s "Y/n" works fine. What it can't
// host is a FULL-SCREEN curses program: `vim`, `htop`, `mc`, `watch` either
// fail outright for want of a TTY or spray escape sequences into the text.
//
// Note that the test is NOT "does it run forever" - `ping` runs forever and
// works perfectly today. It is "does it need a real terminal".
//
// So those programs are handed to a native terminal emulator instead. This
// module is the pure decision-making half of that: which program a command
// line invokes, whether it needs a terminal, and how to invoke whichever
// emulator is installed. No GTK, no fork/exec - see
// ShellSheet::launch_in_terminal() for the half that actually spawns.

// The program a command line invokes, with any directory part stripped.
// Steps past leading `VAR=value` assignments and a `sudo` prefix (including
// sudo's own options and their arguments), so `sudo -u bob /usr/bin/vim f`
// reports "vim". Empty when it can't be determined.
//
// Only the FIRST command on the line is examined. `cd /tmp && vim x` reports
// "cd" - the same conservative stance is_cd_command() takes, rather than
// guessing at shell grammar.
std::string command_program(const std::string& command);

// True only when command_program() names a known full-screen program.
// Matching is exact, never by prefix: "vim" yes, "vimeo" no.
//
// Pipes and redirects are deliberately not inspected, so `htop | cat` still
// goes to a terminal. Capturing a curses program's output is not a thing
// anyone means to do.
bool needs_terminal(const std::string& command);

// An installed terminal emulator and the arguments that precede the command.
// Every emulator listed takes the command as ordinary argv entries, so
// nothing here needs shell quoting.
struct TerminalEmulator {
    std::string binary;
    std::vector<std::string> pre_args;
};

// Picks an emulator: `terminal_env` ($TERMINAL) first if set and available,
// then a built-in priority list. `is_available` answers "is this binary on
// PATH", injected so tests need not touch the filesystem. Returns false when
// nothing suitable is installed.
bool find_terminal_emulator(const std::string& terminal_env,
                             const std::function<bool(const std::string&)>& is_available,
                             TerminalEmulator& out);

// The full argv to exec: binary, its pre_args, then bash -c <shell_command>.
std::vector<std::string> build_terminal_argv(const TerminalEmulator& terminal,
                                              const std::string& shell_command);

#endif // TERMINAL_LAUNCH_H
