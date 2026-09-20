#ifndef SYNTAX_HIGHLIGHT_H
#define SYNTAX_HIGHLIGHT_H

#include <string>
#include <vector>

// Kinds of shell-script token this lightweight highlighter recognizes.
enum class TokenKind { Comment, String, Keyword, Variable };

struct HighlightSpan {
    int line;       // 0-based line number
    int start_col;  // 0-based column, start of the span (byte offset within the line)
    int end_col;    // exclusive end column
    TokenKind kind;

    bool operator==(const HighlightSpan& other) const {
        return line == other.line && start_col == other.start_col &&
               end_col == other.end_col && kind == other.kind;
    }
};

// Scans shell-script text and returns highlight spans for '#' comments,
// single/double-quoted strings, a fixed set of shell keywords, and
// $variables / ${variables}.
//
// This is a lightweight, line-oriented highlighter, not a real shell lexer.
// Known, deliberate limitations:
//   - Quoted strings never span multiple lines; an unterminated quote just
//     highlights to the end of its own line.
//   - Variables are not detected inside quotes of either kind, even though
//     real bash expands them inside double quotes. Treating both quote
//     kinds the same keeps the scanner simple.
std::vector<HighlightSpan> compute_highlight_spans(const std::string& text);

#endif // SYNTAX_HIGHLIGHT_H
