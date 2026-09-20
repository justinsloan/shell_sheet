#include "text_search.h"

#include <glibmm/regex.h>

#include <sstream>

std::vector<SearchMatch> find_matches(const std::string& text,
                                       const std::string& pattern,
                                       const SearchOptions& options) {
    std::vector<SearchMatch> matches;
    if (pattern.empty()) {
        return matches;
    }

    // Both plain and regex search go through Glib::Regex: plain mode just
    // escapes the pattern first, so there's exactly one matching semantics
    // (case handling, Unicode, ...) rather than two separate code paths.
    std::string effective_pattern =
        options.use_regex ? pattern : std::string(Glib::Regex::escape_string(pattern));

    Glib::RefPtr<Glib::Regex> regex;
    try {
        regex = Glib::Regex::create(
            effective_pattern,
            options.case_sensitive ? static_cast<Glib::RegexCompileFlags>(0)
                                    : Glib::REGEX_CASELESS);
    } catch (const Glib::RegexError& e) {
        throw InvalidPatternError(e.what());
    }

    std::istringstream stream(text);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line)) {
        int len = static_cast<int>(line.size());
        int pos = 0;
        while (pos <= len) {
            Glib::MatchInfo match_info;
            if (!regex->match(line, pos, match_info)) {
                break;
            }
            int start = 0, end = 0;
            match_info.fetch_pos(0, start, end);
            matches.push_back({line_no, start, end});
            // A zero-width match (e.g. "x*" matching nothing) wouldn't
            // otherwise advance `pos`, looping forever - step forward by at
            // least one byte in that case.
            pos = (end > pos) ? end : pos + 1;
        }
        ++line_no;
    }

    return matches;
}
