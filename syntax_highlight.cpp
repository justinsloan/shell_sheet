#include "syntax_highlight.h"

#include <cctype>
#include <sstream>
#include <unordered_set>

namespace {

const std::unordered_set<std::string>& keyword_set() {
    static const std::unordered_set<std::string> keywords = {
        "if", "then", "else", "elif", "fi",
        "for", "while", "until", "do", "done",
        "case", "esac", "in", "function", "select",
        "return", "local", "export", "readonly",
        "break", "continue",
    };
    return keywords;
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Special single-character shell parameters, e.g. $?, $@, $#, $$, $!, $*, $-.
bool is_special_param_char(char c) {
    return c == '?' || c == '@' || c == '#' || c == '$' || c == '!' ||
           c == '*' || c == '-';
}

// Consumes a quoted string starting at `line[start]` (which must be a quote
// character). Returns the exclusive end column. Single quotes don't support
// escaping in shell; double quotes treat `\"` as an escaped quote.
int scan_quoted_string(const std::string& line, int start) {
    char quote = line[start];
    int i = start + 1;
    int len = static_cast<int>(line.size());
    while (i < len) {
        if (quote == '"' && line[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (line[i] == quote) {
            return i + 1;
        }
        ++i;
    }
    return len; // unterminated: run to end of line
}

} // namespace

std::vector<HighlightSpan> compute_highlight_spans(const std::string& text) {
    std::vector<HighlightSpan> spans;

    std::istringstream stream(text);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line)) {
        int len = static_cast<int>(line.size());
        int i = 0;
        while (i < len) {
            char c = line[i];

            if (c == '#') {
                spans.push_back({line_no, i, len, TokenKind::Comment});
                break; // rest of the line is the comment
            }

            if (c == '\'' || c == '"') {
                int end = scan_quoted_string(line, i);
                spans.push_back({line_no, i, end, TokenKind::String});
                i = end;
                continue;
            }

            if (c == '$' && i + 1 < len) {
                char next = line[i + 1];
                if (next == '{') {
                    auto close = line.find('}', i + 2);
                    int end = (close == std::string::npos)
                                  ? len
                                  : static_cast<int>(close) + 1;
                    spans.push_back({line_no, i, end, TokenKind::Variable});
                    i = end;
                    continue;
                }
                if (is_special_param_char(next) ||
                    std::isdigit(static_cast<unsigned char>(next))) {
                    spans.push_back({line_no, i, i + 2, TokenKind::Variable});
                    i += 2;
                    continue;
                }
                if (is_word_char(next) && !std::isdigit(static_cast<unsigned char>(next))) {
                    int j = i + 1;
                    while (j < len && is_word_char(line[j])) ++j;
                    spans.push_back({line_no, i, j, TokenKind::Variable});
                    i = j;
                    continue;
                }
                ++i;
                continue;
            }

            bool at_word_start =
                (std::isalpha(static_cast<unsigned char>(c)) || c == '_') &&
                (i == 0 || !is_word_char(line[i - 1]));
            if (at_word_start) {
                int j = i;
                while (j < len && is_word_char(line[j])) ++j;
                std::string word = line.substr(i, j - i);
                if (keyword_set().count(word)) {
                    spans.push_back({line_no, i, j, TokenKind::Keyword});
                }
                i = j;
                continue;
            }

            ++i;
        }
        ++line_no;
    }

    return spans;
}
