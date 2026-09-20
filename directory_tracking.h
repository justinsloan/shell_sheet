#ifndef DIRECTORY_TRACKING_H
#define DIRECTORY_TRACKING_H

#include <string>

// Every command this app runs is a fresh `bash -c`, so a `cd` inside one
// dies with it. To give the worksheet a working directory that persists,
// `cd` lines are recognized and resolved here instead of being handed to
// the shell, and the resolved directory is then applied to every later
// command (see ShellSheet::run_cd_command / on_menu_terminal).
//
// Why resolve it ourselves rather than ask the shell where it ended up:
// asking would mean appending `pwd` to the user's command, which turns a
// one-command `bash -c` into a multi-command one. Bash only exec-replaces
// itself when it has a single command to run, so that would leave bash
// alive as a parent and the real command as a grandchild - and Terminate
// (Ctrl+C), which signals the direct child, would no longer reach it.

// True only for a line that is *nothing but* a cd: first word `cd`, and no
// shell syntax that would make it something more. Deliberately false for
// `cd foo && make`, `cd $DIR`, `cd *`, and anything with a pipe, a
// semicolon, a subshell or a backtick - those still run through the shell
// exactly as before (so they behave correctly), they just don't update the
// tracked directory, rather than us guessing wrong about where they went.
bool is_cd_command(const std::string& command);

// The part after `cd`, trimmed. Empty when `cd` was given on its own.
// Only meaningful when is_cd_command() is true.
std::string cd_argument(const std::string& command);

// Turns a cd argument into the absolute path it refers to. Handles an
// empty argument (home), `-` (the previous directory), a leading `~` or
// `~/...` (home), surrounding single or double quotes, and relative paths
// (resolved against current_dir). `.` and `..` segments are collapsed
// lexically.
//
// Returns an empty string when the target can't be determined - today only
// `cd -` with no previous directory. The caller is still responsible for
// checking that the result actually exists and is a directory; this
// function is pure string work and never touches the filesystem.
//
// Not handled, by design (is_cd_command rejects these lines): variable
// expansion, globbing, command substitution, and `~user`.
std::string resolve_cd_target(const std::string& current_dir,
                               const std::string& argument,
                               const std::string& home_dir,
                               const std::string& previous_dir);

// Collapses `//`, `.` and `..` segments lexically, and drops a trailing
// slash (except on "/" itself). Purely textual: no symlink resolution, and
// no check that anything exists.
std::string normalize_path(const std::string& path);

#endif // DIRECTORY_TRACKING_H
