// Standalone, GTK-free tests for the shell syntax highlighter's pure
// classification logic. Run via `make test` (see Makefile).
#include "../syntax_highlight.h"

#include <cassert>
#include <iostream>
#include <algorithm>

namespace {

int g_failures = 0;

void expect_spans(const std::string& label, const std::string& text,
                   std::vector<HighlightSpan> expected) {
    auto actual = compute_highlight_spans(text);

    auto by_pos = [](const HighlightSpan& a, const HighlightSpan& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.start_col < b.start_col;
    };
    std::sort(actual.begin(), actual.end(), by_pos);
    std::sort(expected.begin(), expected.end(), by_pos);

    if (actual == expected) {
        std::cout << "PASS: " << label << "\n";
        return;
    }

    ++g_failures;
    std::cout << "FAIL: " << label << "\n";
    std::cout << "  text: " << text << "\n";
    std::cout << "  expected " << expected.size() << " span(s):\n";
    for (auto& s : expected) {
        std::cout << "    line=" << s.line << " [" << s.start_col << ","
                   << s.end_col << ") kind=" << static_cast<int>(s.kind) << "\n";
    }
    std::cout << "  actual " << actual.size() << " span(s):\n";
    for (auto& s : actual) {
        std::cout << "    line=" << s.line << " [" << s.start_col << ","
                   << s.end_col << ") kind=" << static_cast<int>(s.kind) << "\n";
    }
}

} // namespace

int main() {
    using K = TokenKind;

    expect_spans("plain comment", "echo hi # a comment", {
        {0, 8, 19, K::Comment},
    });

    expect_spans("comment inside a double-quoted string is not a comment",
        "echo \"a # b\"", {
        {0, 5, 12, K::String},
    });

    expect_spans("keywords are recognized as whole words", "if true; then echo hi; fi", {
        {0, 0, 2, K::Keyword},  // if
        {0, 9, 13, K::Keyword}, // then
        {0, 23, 25, K::Keyword}, // fi
    });

    expect_spans("keyword-like prefix inside a longer identifier is not a keyword",
        "iffy=1", {});

    expect_spans("simple and braced variables, plus a special parameter",
        "echo $HOME ${USER}_x $1 $#", {
        {0, 5, 10, K::Variable},  // $HOME
        {0, 11, 18, K::Variable}, // ${USER}
        {0, 21, 23, K::Variable}, // $1
        {0, 24, 26, K::Variable}, // $#
    });

    expect_spans("variables are not expanded inside quotes of either kind",
        "echo 'a $HOME b' \"c $USER d\"", {
        {0, 5, 16, K::String},
        {0, 17, 28, K::String},
    });

    expect_spans("an unterminated quote highlights to end of line", "echo 'oops", {
        {0, 5, 10, K::String},
    });

    // Single quotes don't support escaping in shell, so 'a\' closes right
    // after the backslash, leaving a bare b and then a second, unterminated
    // single-quoted string starting at the trailing ' - matching real shell
    // parsing, odd as it looks.
    expect_spans("single-quoted strings do not support backslash escapes",
        R"(echo 'a\'b')", {
        {0, 5, 9, K::String},   // 'a\'
        {0, 10, 11, K::String}, // trailing, unterminated '
    });

    expect_spans("double-quoted strings support backslash-escaped quotes",
        R"(echo "a\"b")", {
        {0, 5, 11, K::String}, // "a\"b"
    });

    expect_spans("keywords across multiple lines carry the right line numbers",
        "if true\nthen\n  echo hi\nfi\n", {
        {0, 0, 2, K::Keyword},
        {1, 0, 4, K::Keyword},
        {3, 0, 2, K::Keyword},
    });

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All syntax highlight tests passed\n";
    return 0;
}
