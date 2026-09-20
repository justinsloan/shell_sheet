#include "text_transforms.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

std::string to_upper(const std::string& text) {
    std::string result = text;
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string to_lower(const std::string& text) {
    std::string result = text;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string to_title_case(const std::string& text) {
    std::string result = text;
    bool at_word_start = true;
    for (char& c : result) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            c = static_cast<char>(at_word_start ? std::toupper(uc) : std::tolower(uc));
            at_word_start = false;
        } else {
            at_word_start = true;
        }
    }
    return result;
}

namespace {

// Splits `text` into lines on '\n', reporting whether the input ended with
// a trailing newline (so callers can reproduce that on rejoin).
std::vector<std::string> split_lines(const std::string& text, bool& had_trailing_newline) {
    std::vector<std::string> lines;
    had_trailing_newline = !text.empty() && text.back() == '\n';

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_newline) {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size() || trailing_newline) {
            result += '\n';
        }
    }
    return result;
}

} // namespace

std::string sort_lines(const std::string& text, bool case_insensitive, bool descending) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);

    // One comparator, flipped for descending, rather than two sorts: keeps
    // "descending" exactly the mirror of "ascending" for every case-mode.
    auto less = [case_insensitive](const std::string& a, const std::string& b) {
        return case_insensitive ? (to_lower(a) < to_lower(b)) : (a < b);
    };
    std::stable_sort(lines.begin(), lines.end(),
                      [&](const std::string& a, const std::string& b) {
                          return descending ? less(b, a) : less(a, b);
                      });

    return join_lines(lines, had_trailing_newline);
}

std::string trim_trailing_whitespace(const std::string& text) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);

    for (std::string& line : lines) {
        // A trailing '\r' (CRLF line ending, already split off '\n' by
        // split_lines) is the line terminator, not whitespace to strip.
        bool has_cr = !line.empty() && line.back() == '\r';
        std::string body = has_cr ? line.substr(0, line.size() - 1) : line;

        size_t last_non_ws = body.find_last_not_of(" \t");
        body = (last_non_ws == std::string::npos) ? "" : body.substr(0, last_non_ws + 1);

        line = has_cr ? body + "\r" : body;
    }

    return join_lines(lines, had_trailing_newline);
}

namespace {

// Length of the run of leading spaces/tabs at the start of `line`.
size_t leading_whitespace_length(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return i;
}

} // namespace

std::string tabs_to_spaces(const std::string& text, int tab_width) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    std::string spaces(tab_width, ' ');

    for (std::string& line : lines) {
        size_t indent_len = leading_whitespace_length(line);
        std::string new_indent;
        for (size_t i = 0; i < indent_len; ++i) {
            if (line[i] == '\t') {
                new_indent += spaces;
            } else {
                new_indent += ' ';
            }
        }
        line = new_indent + line.substr(indent_len);
    }

    return join_lines(lines, had_trailing_newline);
}

std::string spaces_to_tabs(const std::string& text, int tab_width) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);

    for (std::string& line : lines) {
        size_t indent_len = leading_whitespace_length(line);
        std::string indent = line.substr(0, indent_len);

        // Only pure-space runs get folded into tabs; a tab already present
        // in the indentation breaks the run at that point, so we don't try
        // to re-align mixed tab/space indentation.
        size_t space_run = indent.find_first_not_of(' ');
        size_t convertible = (space_run == std::string::npos) ? indent.size() : space_run;

        std::string new_indent(convertible / tab_width, '\t');
        new_indent += std::string(convertible % tab_width, ' ');
        new_indent += indent.substr(convertible);

        line = new_indent + line.substr(indent_len);
    }

    return join_lines(lines, had_trailing_newline);
}

std::string indent_lines(const std::string& text, int spaces) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    const std::string prefix(spaces, ' ');

    for (std::string& line : lines) {
        // An empty line stays empty - see the header for why.
        if (!line.empty()) {
            line = prefix + line;
        }
    }

    return join_lines(lines, had_trailing_newline);
}

std::string dedent_lines(const std::string& text, int spaces) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);

    for (std::string& line : lines) {
        if (line.empty()) {
            continue;
        }
        if (line[0] == '\t') {
            line = line.substr(1); // one tab is one indent level
            continue;
        }
        // Remove up to `spaces` leading spaces, stopping early at anything
        // else (including a tab) so we never eat into the line's content.
        size_t removed = 0;
        while (removed < static_cast<size_t>(spaces) && removed < line.size() &&
               line[removed] == ' ') {
            ++removed;
        }
        line = line.substr(removed);
    }

    return join_lines(lines, had_trailing_newline);
}

std::string duplicate_lines(const std::string& text) {
    if (text.empty()) return text;
    // A range that already ends in its line terminator repeats as-is;
    // one that doesn't needs a separator inserted between the copies.
    if (text.back() == '\n') return text + text;
    return text + "\n" + text;
}

std::string move_first_line_to_end(const std::string& text) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    if (lines.size() < 2) return text;

    std::string first = lines.front();
    lines.erase(lines.begin());
    lines.push_back(first);
    return join_lines(lines, had_trailing_newline);
}

std::string move_last_line_to_start(const std::string& text) {
    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    if (lines.size() < 2) return text;

    std::string last = lines.back();
    lines.pop_back();
    lines.insert(lines.begin(), last);
    return join_lines(lines, had_trailing_newline);
}

std::string hard_wrap(const std::string& text, int width) {
    if (width <= 0) return text;

    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    std::vector<std::string> wrapped;

    for (const std::string& line : lines) {
        // Short lines pass through byte-for-byte. Re-flowing them would
        // collapse runs of spaces and destroy aligned command output for
        // no benefit, since they already fit.
        if (line.size() <= static_cast<size_t>(width)) {
            wrapped.push_back(line);
            continue;
        }

        const std::string indent = line.substr(0, leading_whitespace_length(line));
        std::string current = indent;
        bool current_has_word = false;

        size_t i = indent.size();
        while (i < line.size()) {
            size_t word_end = line.find_first_of(" \t", i);
            if (word_end == std::string::npos) word_end = line.size();
            std::string word = line.substr(i, word_end - i);
            i = line.find_first_not_of(" \t", word_end);
            if (i == std::string::npos) i = line.size();

            if (word.empty()) continue;

            if (!current_has_word) {
                // First word on this output line goes on regardless of
                // length - a single over-long token is left intact rather
                // than chopped in half.
                current += word;
                current_has_word = true;
            } else if (current.size() + 1 + word.size() <= static_cast<size_t>(width)) {
                current += " " + word;
            } else {
                wrapped.push_back(current);
                current = indent + word;
            }
        }
        wrapped.push_back(current);
    }

    return join_lines(wrapped, had_trailing_newline);
}

std::string clamp_lines(const std::string& text, int max_lines) {
    if (text.empty()) return text;

    bool had_trailing_newline;
    std::vector<std::string> lines = split_lines(text, had_trailing_newline);
    // The trailing newline is deliberately not carried through: this is
    // display text, and it would show up as an empty last line.
    (void)had_trailing_newline;

    size_t keep = (max_lines > 0) ? static_cast<size_t>(max_lines) : 0;
    if (lines.size() <= keep) {
        return join_lines(lines, /*trailing_newline=*/false);
    }

    lines.resize(keep);
    lines.push_back("...");
    return join_lines(lines, /*trailing_newline=*/false);
}

std::string convert_line_endings(const std::string& text, LineEnding target) {
    std::string normalized;
    normalized.reserve(text.size());
    // First pass: normalize every existing style (CRLF, lone CR, lone LF)
    // down to a single '\n' per line break.
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            normalized += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i; // consume the LF half of a CRLF pair
            }
        } else {
            normalized += c;
        }
    }

    if (target == LineEnding::LF) {
        return normalized;
    }

    std::string result;
    result.reserve(normalized.size());
    const std::string terminator = (target == LineEnding::CRLF) ? "\r\n" : "\r";
    for (char c : normalized) {
        if (c == '\n') {
            result += terminator;
        } else {
            result += c;
        }
    }
    return result;
}
