#include "directory_tracking.h"

#include <vector>

namespace {

std::string trim(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Shell syntax that would make a line more than a simple `cd`, or that we
// would have to expand ourselves to know the destination. Either way we
// hand the line to the shell untouched rather than guess - see the header.
bool has_shell_syntax(const std::string& s) {
    return s.find_first_of("&|;<>()`$*?") != std::string::npos;
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

} // namespace

bool is_cd_command(const std::string& command) {
    std::string trimmed = trim(command);
    if (trimmed.empty()) return false;

    // First word must be exactly "cd" - not "cdrecord", not "mycd".
    if (trimmed.rfind("cd", 0) != 0) return false;
    if (trimmed.size() > 2 && trimmed[2] != ' ' && trimmed[2] != '\t') return false;

    if (has_shell_syntax(trimmed)) return false;

    // `~user` needs the password database to resolve; `~` and `~/...` don't.
    std::string arg = strip_quotes(trim(trimmed.substr(2)));
    if (arg.size() > 1 && arg[0] == '~' && arg[1] != '/') return false;

    return true;
}

std::string cd_argument(const std::string& command) {
    std::string trimmed = trim(command);
    if (trimmed.size() <= 2) return "";
    return trim(trimmed.substr(2));
}

std::string normalize_path(const std::string& path) {
    bool absolute = !path.empty() && path.front() == '/';

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        std::string segment = path.substr(i, slash - i);
        i = slash + 1;

        if (segment.empty() || segment == ".") {
            continue; // "//" and "/./" contribute nothing
        }
        if (segment == "..") {
            // Never climb above the root: "/../.." is still "/", matching
            // how the kernel treats it.
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back("..");
            }
            continue;
        }
        parts.push_back(segment);
    }

    std::string result;
    for (const std::string& part : parts) {
        if (!result.empty() || absolute) result += "/";
        result += part;
    }
    if (result.empty()) return absolute ? "/" : ".";
    return result;
}

std::string resolve_cd_target(const std::string& current_dir, const std::string& argument,
                               const std::string& home_dir, const std::string& previous_dir) {
    std::string arg = strip_quotes(trim(argument));

    if (arg.empty()) {
        return normalize_path(home_dir);
    }
    if (arg == "-") {
        // Nothing to go back to yet - the caller reports this, rather than
        // silently treating it as "stay here".
        if (previous_dir.empty()) return "";
        return normalize_path(previous_dir);
    }
    if (arg == "~") {
        return normalize_path(home_dir);
    }
    if (arg.rfind("~/", 0) == 0) {
        return normalize_path(home_dir + "/" + arg.substr(2));
    }
    if (arg.front() == '/') {
        return normalize_path(arg);
    }
    return normalize_path(current_dir + "/" + arg);
}
