#ifndef TEXT_SEARCH_H
#define TEXT_SEARCH_H

#include <stdexcept>
#include <string>
#include <vector>

// Mirrors HighlightSpan's shape on purpose (see syntax_highlight.h): 0-based
// line, half-open [start_col, end_col) BYTE offsets within that line - the
// same unit Gtk::TextBuffer::get_iter_at_line_index() expects, so mapping a
// match to a Gtk::TextIter needs no char-vs-byte conversion.
struct SearchMatch {
    int line;
    int start_col;
    int end_col;

    bool operator==(const SearchMatch& other) const {
        return line == other.line && start_col == other.start_col &&
               end_col == other.end_col;
    }
};

struct SearchOptions {
    bool case_sensitive = false;
    bool use_regex = false;
};

// Thrown only when options.use_regex is true and pattern fails to compile.
// what() is GLib's own compiler diagnostic, passed through unchanged.
struct InvalidPatternError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Finds every non-overlapping match of `pattern` in `text`, scanning line by
// line like compute_highlight_spans() does - so, like that function, a
// pattern (even in regex mode) can never match across a line boundary.
//
// An empty `pattern` always yields zero matches, checked before any regex
// gets compiled, so callers never need to special-case "search for nothing"
// themselves.
//
// In non-regex mode, `pattern` is escaped and matched literally - both
// modes funnel through the same underlying matcher, so case-sensitivity and
// Unicode handling behave identically for plain and regex search rather
// than maintaining two separate code paths.
std::vector<SearchMatch> find_matches(const std::string& text,
                                       const std::string& pattern,
                                       const SearchOptions& options);

#endif // TEXT_SEARCH_H
