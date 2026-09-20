// Standalone, dependency-free tests for the cd-tracking logic.
// Run via `make test`.
#include "../directory_tracking.h"

#include <iostream>

namespace {

int g_failures = 0;

void expect_eq(const std::string& label, const std::string& actual,
                const std::string& expected) {
    if (actual == expected) {
        std::cout << "PASS: " << label << "\n";
        return;
    }
    ++g_failures;
    std::cout << "FAIL: " << label << "\n";
    std::cout << "  expected: [" << expected << "]\n";
    std::cout << "  actual:   [" << actual << "]\n";
}

void expect_true(const std::string& label, bool cond) {
    std::cout << (cond ? "PASS: " : "FAIL: ") << label << "\n";
    if (!cond) ++g_failures;
}

std::string resolve(const std::string& arg, const std::string& cur = "/home/justin",
                     const std::string& home = "/home/justin",
                     const std::string& prev = "/tmp") {
    return resolve_cd_target(cur, arg, home, prev);
}

} // namespace

int main() {
    // --- is_cd_command: the plain cases it must catch ---
    expect_true("bare cd", is_cd_command("cd"));
    expect_true("cd with a path", is_cd_command("cd /usr/local"));
    expect_true("cd with leading whitespace", is_cd_command("   cd /tmp"));
    expect_true("cd with trailing whitespace", is_cd_command("cd /tmp   "));
    expect_true("cd with a quoted path", is_cd_command("cd \"/my dir\""));
    expect_true("cd -", is_cd_command("cd -"));
    expect_true("cd ~", is_cd_command("cd ~/projects"));
    expect_true("cd ..", is_cd_command("cd .."));

    // --- is_cd_command: things that merely start with the letters c,d ---
    expect_true("not 'cdrecord'", !is_cd_command("cdrecord foo"));
    expect_true("not 'cd' inside a word", !is_cd_command("mycd /tmp"));
    expect_true("not a different command", !is_cd_command("ls -la"));
    expect_true("not an empty line", !is_cd_command(""));
    expect_true("not whitespace only", !is_cd_command("   "));

    // --- is_cd_command: compound lines stay with the shell ---
    // These must run normally (so they still WORK); they just don't update
    // the tracked directory, because we can't know where they ended up.
    expect_true("not 'cd x && make'", !is_cd_command("cd /tmp && make"));
    expect_true("not 'cd x; ls'", !is_cd_command("cd /tmp; ls"));
    expect_true("not 'cd x || y'", !is_cd_command("cd /tmp || echo no"));
    expect_true("not a pipe", !is_cd_command("cd /tmp | tee log"));
    expect_true("not backgrounded", !is_cd_command("cd /tmp &"));
    expect_true("not a variable", !is_cd_command("cd $HOME"));
    expect_true("not a brace variable", !is_cd_command("cd ${HOME}/x"));
    expect_true("not a glob", !is_cd_command("cd /tmp/*"));
    expect_true("not a '?' glob", !is_cd_command("cd /tmp/a?b"));
    expect_true("not command substitution", !is_cd_command("cd $(pwd)"));
    expect_true("not a backtick", !is_cd_command("cd `pwd`"));
    expect_true("not a redirect", !is_cd_command("cd /tmp > log"));
    expect_true("not ~user", !is_cd_command("cd ~root"));

    // --- cd_argument ---
    expect_eq("argument of bare cd", cd_argument("cd"), "");
    expect_eq("argument of 'cd /tmp'", cd_argument("cd /tmp"), "/tmp");
    expect_eq("argument is trimmed", cd_argument("  cd   /tmp  "), "/tmp");
    expect_eq("argument of 'cd -'", cd_argument("cd -"), "-");

    // --- resolve_cd_target ---
    expect_eq("empty argument goes home", resolve(""), "/home/justin");
    expect_eq("'-' goes to the previous directory", resolve("-"), "/tmp");
    expect_eq("'-' with no previous directory fails",
              resolve("-", "/home/justin", "/home/justin", ""), "");
    expect_eq("absolute path is used as-is", resolve("/usr/local"), "/usr/local");
    expect_eq("relative path joins the current directory",
              resolve("projects"), "/home/justin/projects");
    expect_eq("'..' goes up", resolve("..", "/home/justin/projects"), "/home/justin");
    expect_eq("'.' stays put", resolve(".", "/home/justin"), "/home/justin");
    expect_eq("nested relative path", resolve("a/b/c"), "/home/justin/a/b/c");
    expect_eq("relative path with '..' in it",
              resolve("../other", "/home/justin/projects"), "/home/justin/other");
    expect_eq("'~' alone is home", resolve("~"), "/home/justin");
    expect_eq("'~/x' is under home", resolve("~/projects"), "/home/justin/projects");
    expect_eq("double-quoted path", resolve("\"/my dir\""), "/my dir");
    expect_eq("single-quoted path", resolve("'/my dir'"), "/my dir");
    expect_eq("quoted relative path", resolve("\"my dir\""), "/home/justin/my dir");
    expect_eq("cd from root", resolve("usr", "/"), "/usr");
    expect_eq("cannot go above root", resolve("..", "/"), "/");
    expect_eq("trailing slash is dropped", resolve("/usr/local/"), "/usr/local");

    // --- normalize_path ---
    expect_eq("collapses duplicate slashes", normalize_path("/usr//local"), "/usr/local");
    expect_eq("removes '.' segments", normalize_path("/usr/./local"), "/usr/local");
    expect_eq("resolves '..' segments", normalize_path("/usr/local/../bin"), "/usr/bin");
    expect_eq("resolves several '..'", normalize_path("/a/b/c/../../d"), "/a/d");
    expect_eq("'..' cannot escape root", normalize_path("/../.."), "/");
    expect_eq("root stays root", normalize_path("/"), "/");
    expect_eq("trailing slash dropped", normalize_path("/usr/"), "/usr");
    expect_eq("path with spaces survives", normalize_path("/my dir/sub"), "/my dir/sub");

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All directory tracking tests passed\n";
    return 0;
}
