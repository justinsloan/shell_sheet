#include "undo_stack.h"

namespace {

constexpr long kMergeWindowMs = 700;

} // namespace

bool try_merge_edit(EditRecord& prev, const EditRecord& new_record, long elapsed_ms,
                     bool force_new_group) {
    if (force_new_group) return false;
    if (prev.kind != new_record.kind) return false;
    if (elapsed_ms > kMergeWindowMs) return false;

    if (new_record.kind == EditRecord::Kind::Insert) {
        // Typing landed exactly where the previous insert ended.
        if (new_record.offset != prev.offset + static_cast<int>(prev.text.length())) {
            return false;
        }
        prev.text += new_record.text;
        return true;
    }

    // Delete: merges in either direction, matching how Backspace (deleting
    // backward, cursor moves left) and the Delete key (deleting forward,
    // cursor doesn't move) each behave.
    int new_len = static_cast<int>(new_record.text.length());
    if (new_record.offset + new_len == prev.offset) {
        // Backspace-direction: new deletion's range ends exactly where the
        // previous deletion's range started - prepend.
        prev.offset = new_record.offset;
        prev.text = new_record.text + prev.text;
        return true;
    }
    if (new_record.offset == prev.offset) {
        // Delete-key-direction: new deletion happens at the same offset
        // (the next character slid into place) - append.
        prev.text += new_record.text;
        return true;
    }

    return false;
}
