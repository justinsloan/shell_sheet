// Standalone, dependency-free tests for the pure text-transformation logic.
// Run via `make test`.
#include "../text_transforms.h"

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
    std::cout << "  expected: " << expected << "\n";
    std::cout << "  actual:   " << actual << "\n";
}

} // namespace

int main() {
    // --- case conversion ---
    expect_eq("to_upper basic", to_upper("Hello, World!"), "HELLO, WORLD!");
    expect_eq("to_lower basic", to_lower("Hello, World!"), "hello, world!");
    expect_eq("to_upper/to_lower on empty string", to_upper(""), "");
    expect_eq("to_upper leaves non-ASCII untouched", to_upper("caf\xc3\xa9"), "CAF\xc3\xa9");

    expect_eq("to_title_case basic", to_title_case("the quick brown fox"),
              "The Quick Brown Fox");
    expect_eq("to_title_case does not skip small words (deliberate, documented)",
              to_title_case("a tale of two cities"), "A Tale Of Two Cities");
    expect_eq("to_title_case lowercases the rest of each word",
              to_title_case("HELLO WORLD"), "Hello World");
    expect_eq("to_title_case on empty string", to_title_case(""), "");

    // --- sort_lines ---
    expect_eq("sort_lines basic", sort_lines("banana\napple\ncherry"),
              "apple\nbanana\ncherry");
    expect_eq("sort_lines preserves absence of trailing newline",
              sort_lines("b\na"), "a\nb");
    expect_eq("sort_lines preserves presence of trailing newline",
              sort_lines("b\na\n"), "a\nb\n");
    expect_eq("sort_lines case-sensitive by default (uppercase sorts first)",
              sort_lines("banana\nApple"), "Apple\nbanana");
    expect_eq("sort_lines case-insensitive when requested",
              sort_lines("banana\nApple", /*case_insensitive=*/true), "Apple\nbanana");
    expect_eq("sort_lines on empty string", sort_lines(""), "");
    expect_eq("sort_lines descending reverses the ascending order",
              sort_lines("banana\napple\ncherry", /*case_insensitive=*/false,
                          /*descending=*/true),
              "cherry\nbanana\napple");
    expect_eq("sort_lines descending preserves trailing newline",
              sort_lines("a\nb\n", false, true), "b\na\n");
    expect_eq("sort_lines descending honors case-insensitivity",
              sort_lines("Apple\nbanana", /*case_insensitive=*/true,
                          /*descending=*/true),
              "banana\nApple");

    // --- indent_lines / dedent_lines ---
    expect_eq("indent_lines adds four spaces to every line",
              indent_lines("foo\nbar"), "    foo\n    bar");
    expect_eq("indent_lines honors a custom width",
              indent_lines("foo", 2), "  foo");
    expect_eq("indent_lines stacks onto existing indentation",
              indent_lines("    foo"), "        foo");
    expect_eq("indent_lines skips empty lines (no whitespace-only lines)",
              indent_lines("foo\n\nbar"), "    foo\n\n    bar");
    expect_eq("indent_lines preserves trailing newline",
              indent_lines("foo\n"), "    foo\n");
    expect_eq("indent_lines on empty string", indent_lines(""), "");

    expect_eq("dedent_lines removes one level of spaces",
              dedent_lines("    foo\n    bar"), "foo\nbar");
    expect_eq("dedent_lines removes a partial level when there is less indentation",
              dedent_lines("  foo"), "foo");
    expect_eq("dedent_lines removes only one level from deeper indentation",
              dedent_lines("        foo"), "    foo");
    expect_eq("dedent_lines removes a single leading tab as one level",
              dedent_lines("\tfoo"), "foo");
    expect_eq("dedent_lines removes only one tab from a double-tab indent",
              dedent_lines("\t\tfoo"), "\tfoo");
    expect_eq("dedent_lines stops at the first tab in mixed indentation",
              dedent_lines("  \tfoo"), "\tfoo");
    expect_eq("dedent_lines leaves unindented lines alone",
              dedent_lines("foo\n    bar"), "foo\nbar");
    expect_eq("dedent_lines leaves an empty line empty",
              dedent_lines("    foo\n\n    bar"), "foo\n\nbar");
    expect_eq("dedent_lines preserves trailing newline",
              dedent_lines("    foo\n"), "foo\n");
    expect_eq("dedent_lines on empty string", dedent_lines(""), "");
    expect_eq("dedent_lines on a whitespace-only line clears it",
              dedent_lines("    "), "");

    // --- trim_trailing_whitespace ---
    expect_eq("trim_trailing_whitespace basic",
              trim_trailing_whitespace("foo   \nbar\t\n"), "foo\nbar\n");
    expect_eq("trim_trailing_whitespace preserves CRLF line endings",
              trim_trailing_whitespace("foo   \r\nbar\r\n"), "foo\r\nbar\r\n");
    expect_eq("trim_trailing_whitespace on a line with no trailing newline",
              trim_trailing_whitespace("foo   "), "foo");
    expect_eq("trim_trailing_whitespace leaves already-clean text alone",
              trim_trailing_whitespace("foo\nbar\n"), "foo\nbar\n");

    // --- tabs_to_spaces / spaces_to_tabs ---
    expect_eq("tabs_to_spaces default width 4", tabs_to_spaces("\tfoo"), "    foo");
    expect_eq("tabs_to_spaces width 2", tabs_to_spaces("\tfoo", 2), "  foo");
    expect_eq("tabs_to_spaces multiple tabs", tabs_to_spaces("\t\tfoo"), "        foo");
    expect_eq("spaces_to_tabs default width 4", spaces_to_tabs("    foo"), "\tfoo");
    expect_eq("spaces_to_tabs leaves a partial indent as spaces",
              spaces_to_tabs("      foo"), "\t  foo"); // 6 spaces -> one tab + 2 leftover spaces
    expect_eq("spaces_to_tabs only touches leading indentation, not spaces mid-line",
              spaces_to_tabs("    foo    bar"), "\tfoo    bar");

    // --- duplicate_lines ---
    expect_eq("duplicate a single line", duplicate_lines("foo"), "foo\nfoo");
    expect_eq("duplicate several lines", duplicate_lines("a\nb"), "a\nb\na\nb");
    expect_eq("duplicate respects a trailing newline rather than doubling it",
              duplicate_lines("foo\n"), "foo\nfoo\n");
    expect_eq("duplicate several lines with a trailing newline",
              duplicate_lines("a\nb\n"), "a\nb\na\nb\n");
    expect_eq("duplicate an empty line", duplicate_lines(""), "");
    expect_eq("duplicate a blank line", duplicate_lines("\n"), "\n\n");
    expect_eq("duplicate preserves indentation", duplicate_lines("    x"), "    x\n    x");

    // --- move_first_line_to_end / move_last_line_to_start ---
    expect_eq("move first line to the end", move_first_line_to_end("a\nb"), "b\na");
    expect_eq("move first line past a multi-line block",
              move_first_line_to_end("above\nx\ny"), "x\ny\nabove");
    expect_eq("move last line to the start", move_last_line_to_start("a\nb"), "b\na");
    expect_eq("move last line ahead of a multi-line block",
              move_last_line_to_start("x\ny\nbelow"), "below\nx\ny");
    expect_eq("rotating is a no-op on a single line", move_first_line_to_end("only"), "only");
    expect_eq("rotating the other way is also a no-op on a single line",
              move_last_line_to_start("only"), "only");
    expect_eq("rotation on empty text", move_first_line_to_end(""), "");
    expect_eq("rotation keeps blank lines", move_first_line_to_end("a\n\nb"), "\nb\na");
    expect_eq("the two rotations undo each other",
              move_last_line_to_start(move_first_line_to_end("a\nb\nc")), "a\nb\nc");

    // --- hard_wrap ---
    expect_eq("a line that fits is untouched", hard_wrap("short line", 20), "short line");
    expect_eq("a line that fits keeps its internal spacing",
              hard_wrap("col1    col2    col3", 40), "col1    col2    col3");
    expect_eq("a long line is broken at a space",
              hard_wrap("aaa bbb ccc ddd", 7), "aaa bbb\nccc ddd");
    expect_eq("breaking happens at the last space that fits",
              hard_wrap("one two three", 9), "one two\nthree");
    expect_eq("an over-long single word is left intact, not split",
              hard_wrap("https://example.com/very/long/path", 10),
              "https://example.com/very/long/path");
    expect_eq("an over-long word still separates from its neighbours",
              hard_wrap("hi https://example.com/very/long/path there", 10),
              "hi\nhttps://example.com/very/long/path\nthere");
    expect_eq("indentation is carried onto continuation lines",
              hard_wrap("    aaa bbb ccc", 11), "    aaa bbb\n    ccc");
    expect_eq("each line is wrapped independently (paragraphs are not joined)",
              hard_wrap("aaa bbb ccc\nshort", 7), "aaa bbb\nccc\nshort");
    expect_eq("blank lines survive", hard_wrap("aaa bbb\n\nccc", 3), "aaa\nbbb\n\nccc");
    expect_eq("trailing newline is preserved", hard_wrap("aaa bbb\n", 3), "aaa\nbbb\n");
    expect_eq("hard_wrap on empty text", hard_wrap("", 80), "");
    expect_eq("a nonsensical width leaves the text alone",
              hard_wrap("aaa bbb ccc", 0), "aaa bbb ccc");

    // --- clamp_lines ---
    expect_eq("fewer lines than the limit are untouched",
              clamp_lines("a\nb", 3), "a\nb");
    expect_eq("exactly the limit is untouched, with no ellipsis",
              clamp_lines("a\nb\nc", 3), "a\nb\nc");
    expect_eq("one line over the limit gains an ellipsis line",
              clamp_lines("a\nb\nc\nd", 3), "a\nb\nc\n...");
    expect_eq("many extra lines still add exactly one ellipsis line",
              clamp_lines("a\nb\nc\nd\ne\nf", 3), "a\nb\nc\n...");
    expect_eq("a single line is untouched", clamp_lines("only", 3), "only");
    expect_eq("a trailing newline does not become an empty final line",
              clamp_lines("a\nb\n", 3), "a\nb");
    expect_eq("a trailing newline on a full-length text is still dropped",
              clamp_lines("a\nb\nc\n", 3), "a\nb\nc");
    expect_eq("blank lines count as lines", clamp_lines("a\n\n\nd", 3), "a\n\n\n...");
    expect_eq("clamp_lines on empty text", clamp_lines("", 3), "");
    expect_eq("a limit of one keeps just the first line",
              clamp_lines("a\nb\nc", 1), "a\n...");
    expect_eq("a zero limit is all ellipsis", clamp_lines("a\nb", 0), "...");
    expect_eq("a negative limit behaves like zero", clamp_lines("a\nb", -1), "...");

    // --- convert_line_endings ---
    expect_eq("convert_line_endings LF to CRLF",
              convert_line_endings("a\nb\nc", LineEnding::CRLF), "a\r\nb\r\nc");
    expect_eq("convert_line_endings CRLF to LF",
              convert_line_endings("a\r\nb\r\nc", LineEnding::LF), "a\nb\nc");
    expect_eq("convert_line_endings LF to CR",
              convert_line_endings("a\nb\nc", LineEnding::CR), "a\rb\rc");
    expect_eq("convert_line_endings CR to LF",
              convert_line_endings("a\rb\rc", LineEnding::LF), "a\nb\nc");
    expect_eq("convert_line_endings normalizes mixed line endings to one style",
              convert_line_endings("a\r\nb\nc\rd", LineEnding::LF), "a\nb\nc\nd");
    expect_eq("convert_line_endings is a no-op when already the target style",
              convert_line_endings("a\nb\nc", LineEnding::LF), "a\nb\nc");

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All text transform tests passed\n";
    return 0;
}
