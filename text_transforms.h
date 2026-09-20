#ifndef TEXT_TRANSFORMS_H
#define TEXT_TRANSFORMS_H

#include <string>

enum class LineEnding { LF, CRLF, CR };

// Case conversion is byte-wise / ASCII-only, the same simplification
// syntax_highlight.cpp already makes for its byte-offset columns. Non-ASCII
// (accented / multibyte UTF-8) text is left untouched, not mangled, but
// also not correctly re-cased.
std::string to_upper(const std::string& text);
std::string to_lower(const std::string& text);

// Capitalizes the first alnum character of each whitespace-delimited word
// and lowercases the rest. Deliberately simple: unlike BBEdit's own Title
// Case, this does not skip capitalizing small words ("a", "the", "of", ...).
std::string to_title_case(const std::string& text);

// Sorts text as whole lines (splitting on '\n'). Preserves whether the
// input ends with a trailing newline.
std::string sort_lines(const std::string& text, bool case_insensitive = false,
                        bool descending = false);

// Strips trailing spaces/tabs from every line. A trailing '\r' (CRLF-style
// line endings) is treated as part of the line terminator, not as
// whitespace to strip.
std::string trim_trailing_whitespace(const std::string& text);

// Both operate only on each line's LEADING whitespace (its indentation),
// never on spaces/tabs elsewhere in the line - converting every space
// anywhere (e.g. between words, or inside quoted data) would corrupt
// content that has nothing to do with indentation style.
//
// spaces_to_tabs converts leading spaces in fixed tab_width-sized chunks;
// a leftover run shorter than tab_width stays as spaces (e.g. 6 leading
// spaces at width 4 becomes one tab plus 2 spaces, not a fractional tab).
std::string tabs_to_spaces(const std::string& text, int tab_width = 4);
std::string spaces_to_tabs(const std::string& text, int tab_width = 4);

// Indentation, one level at a time. Both operate on whole lines.
//
// indent_lines prefixes every line with `spaces` spaces, but skips empty
// lines: indenting a blank line would turn it into a whitespace-only line,
// which trim_trailing_whitespace would then undo - and which no editor's
// Indent command produces.
//
// dedent_lines removes up to one level of leading indentation: either up to
// `spaces` leading spaces, or a single leading tab (whichever comes first),
// so it undoes both this app's own indent and tab-indented files. Lines with
// no leading whitespace are left alone rather than borrowing from the next
// line, so dedenting a block only ever shifts it left, never reflows it.
std::string indent_lines(const std::string& text, int spaces = 4);
std::string dedent_lines(const std::string& text, int spaces = 4);

// Repeats the text below itself, as whole lines: "a\nb" becomes
// "a\nb\na\nb". A trailing newline is respected rather than doubled, so a
// range that includes its line terminator duplicates cleanly instead of
// gaining a blank line in between.
std::string duplicate_lines(const std::string& text);

// The two halves of "move this block of lines up / down by one". The
// caller passes a region that is the block plus the one neighbouring line
// it is trading places with, and these rotate that neighbour to the other
// end. Keeping it this way means the move is a single small edit, rather
// than rewriting the whole document.
std::string move_first_line_to_end(const std::string& text);
std::string move_last_line_to_start(const std::string& text);

// Re-breaks long lines with real newlines so no line exceeds `width`
// columns. Unlike soft wrap this changes the text.
//
// Lines that already fit are left completely untouched - important for
// command output, where collapsing the internal whitespace of an aligned
// table would wreck it. Breaks happen only at whitespace, so a single
// long token (a URL, a path, a base64 blob) is left over-long rather than
// split down the middle. Leading indentation is carried onto each
// continuation line, so indented text stays indented.
std::string hard_wrap(const std::string& text, int width = 80);

// Shortens text for display to at most `max_lines` lines. If anything was
// dropped, one more line holding "..." is added in its place, so the
// result is never longer than max_lines + 1 lines. The result never ends
// in a newline - a trailing one would render as an empty final line.
//
// This is for showing a command in the running indicator, not for editing
// buffer text: nothing here is reversible.
std::string clamp_lines(const std::string& text, int max_lines);

// Normalizes all line-ending styles present (including mixed CRLF/CR/LF) to
// one consistent target style.
std::string convert_line_endings(const std::string& text, LineEnding target);

#endif // TEXT_TRANSFORMS_H
