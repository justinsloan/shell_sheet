#ifndef UNDO_STACK_H
#define UNDO_STACK_H

#include <string>

// See docs/superpowers/specs/2026-09-16-undo-redo-design.md for the full
// design this implements.
//
// Byte vs. character offsets: `offset` here is meant to line up with
// Gtk::TextIter::get_offset(), which counts characters, not bytes - but
// `text.length()` (used wherever an end offset is derived from a start
// offset plus this record's text) is a byte count. For ASCII shell-script
// content the two are identical, matching the same simplification
// syntax_highlight.h and text_search.h already document for the same
// reason. Multi-byte UTF-8 text would make a derived offset land in the
// wrong place.
struct EditRecord {
    enum class Kind { Insert, Delete };
    Kind kind;
    int offset;       // buffer character offset
    std::string text; // inserted text, or the text that was deleted

    // True when this record is the continuation of the record beneath it on
    // the stack, and the two (or more) must undo and redo together as one
    // step. Needed because a "replace" is physically an erase followed by an
    // insert - two records - but is one action to the user. Without this,
    // Indent would take two Ctrl+Z presses, the first of which would leave
    // the text deleted: a visibly broken intermediate state, not a state the
    // document was ever in.
    bool continues_previous = false;

    bool operator==(const EditRecord& other) const {
        return kind == other.kind && offset == other.offset && text == other.text &&
               continues_previous == other.continues_previous;
    }
};

// Pure decision function, no Gtk::TextBuffer/TextIter involved: given the
// previous record on top of the undo stack and a freshly-captured new
// edit (plus milliseconds elapsed since the previous edit), does the new
// edit merge into `prev` in place, or does it need to start a new record?
//
// Returns true and mutates `prev` in place if it merges; returns false
// (leaving `prev` untouched) if the caller should push `new_record` as a
// new entry instead. `force_new_group` short-circuits straight to false,
// bypassing every other check - used by call sites (paste, command
// output, Replace, text transforms, ...) that must never silently merge
// with whatever edit preceded them.
bool try_merge_edit(EditRecord& prev, const EditRecord& new_record,
                     long elapsed_ms, bool force_new_group);

#endif // UNDO_STACK_H
