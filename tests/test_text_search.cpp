// Standalone tests for the pure text-search matching logic. Needs glibmm
// (Glib::Regex), but no GTK/display - run via `make test`.
#include "../text_search.h"

#include <algorithm>
#include <iostream>

namespace {

int g_failures = 0;

void expect_matches(const std::string& label, const std::string& text,
                     const std::string& pattern, SearchOptions options,
                     std::vector<SearchMatch> expected) {
    std::vector<SearchMatch> actual;
    try {
        actual = find_matches(text, pattern, options);
    } catch (const InvalidPatternError& e) {
        std::cout << "FAIL: " << label << " (threw InvalidPatternError: " << e.what() << ")\n";
        ++g_failures;
        return;
    }

    auto by_pos = [](const SearchMatch& a, const SearchMatch& b) {
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
    std::cout << "  expected " << expected.size() << " match(es):\n";
    for (auto& m : expected) {
        std::cout << "    line=" << m.line << " [" << m.start_col << "," << m.end_col << ")\n";
    }
    std::cout << "  actual " << actual.size() << " match(es):\n";
    for (auto& m : actual) {
        std::cout << "    line=" << m.line << " [" << m.start_col << "," << m.end_col << ")\n";
    }
}

void expect_throws_invalid_pattern(const std::string& label, const std::string& pattern) {
    try {
        find_matches("anything", pattern, {false, true});
        std::cout << "FAIL: " << label << " (expected InvalidPatternError, nothing thrown)\n";
        ++g_failures;
    } catch (const InvalidPatternError& e) {
        if (std::string(e.what()).empty()) {
            std::cout << "FAIL: " << label << " (threw, but with an empty message)\n";
            ++g_failures;
        } else {
            std::cout << "PASS: " << label << "\n";
        }
    }
}

} // namespace

int main() {
    expect_matches("plain text, single match", "hello world", "world", {}, {
        {0, 6, 11},
    });

    expect_matches("plain text, multiple matches on one line", "abcabcabc", "abc", {}, {
        {0, 0, 3}, {0, 3, 6}, {0, 6, 9},
    });

    expect_matches("plain text, matches across multiple lines",
        "foo\nbar\nfoo", "foo", {}, {
        {0, 0, 3}, {2, 0, 3},
    });

    expect_matches("empty pattern always yields zero matches", "some text here", "", {}, {});

    expect_matches("case-insensitive by default", "Hello HELLO hello", "hello", {}, {
        {0, 0, 5}, {0, 6, 11}, {0, 12, 17},
    });

    expect_matches("case-sensitive when requested", "Hello HELLO hello", "hello",
        {/*case_sensitive=*/true, false}, {
        {0, 12, 17},
    });

    expect_matches("regex mode: character class", "cat bat hat mat", "[cb]at",
        {false, true}, {
        {0, 0, 3}, {0, 4, 7},
    });

    expect_matches("regex mode: anchors", "foo\nfoobar\nbarfoo", "^foo",
        {false, true}, {
        {0, 0, 3}, {1, 0, 3},
    });

    expect_matches("regex mode: a literal-looking pattern behaves like plain search",
        "3.14 and 3x14", "3.14", {false, true}, {
        {0, 0, 4}, {0, 9, 13}, // '.' matches any char in regex mode, by design
    });

    expect_matches("non-regex mode treats special characters literally",
        "3.14 and 3x14", "3.14", {false, false}, {
        {0, 0, 4}, // only the literal "3.14" matches, not "3x14"
    });

    expect_matches("zero-width matches make progress instead of looping forever",
        "aaa", "x*", {false, true}, {
        {0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 3},
    });

    expect_throws_invalid_pattern("invalid regex throws with a non-empty message", "[unclosed");

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All text search tests passed\n";
    return 0;
}
