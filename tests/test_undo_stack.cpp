// Standalone, dependency-free tests for the pure undo-merge decision
// logic. Run via `make test`. See
// docs/superpowers/specs/2026-09-16-undo-redo-design.md for the design.
#include "../undo_stack.h"

#include <iostream>

namespace {

int g_failures = 0;
using Kind = EditRecord::Kind;

void expect_merge(const std::string& label, EditRecord prev, const EditRecord& new_record,
                   long elapsed_ms, bool force_new_group, const EditRecord& expected_merged) {
    bool merged = try_merge_edit(prev, new_record, elapsed_ms, force_new_group);
    if (!merged) {
        ++g_failures;
        std::cout << "FAIL: " << label << " (expected a merge, got none)\n";
        return;
    }
    if (!(prev == expected_merged)) {
        ++g_failures;
        std::cout << "FAIL: " << label << " (merged, but result differs)\n";
        std::cout << "  expected: kind=" << static_cast<int>(expected_merged.kind)
                   << " offset=" << expected_merged.offset << " text=\"" << expected_merged.text << "\"\n";
        std::cout << "  actual:   kind=" << static_cast<int>(prev.kind)
                   << " offset=" << prev.offset << " text=\"" << prev.text << "\"\n";
        return;
    }
    std::cout << "PASS: " << label << "\n";
}

void expect_no_merge(const std::string& label, EditRecord prev, const EditRecord& new_record,
                      long elapsed_ms, bool force_new_group) {
    EditRecord prev_copy = prev;
    bool merged = try_merge_edit(prev, new_record, elapsed_ms, force_new_group);
    if (merged) {
        ++g_failures;
        std::cout << "FAIL: " << label << " (expected no merge, but it merged)\n";
        return;
    }
    if (!(prev == prev_copy)) {
        ++g_failures;
        std::cout << "FAIL: " << label << " (did not merge, but mutated prev anyway)\n";
        return;
    }
    std::cout << "PASS: " << label << "\n";
}

} // namespace

int main() {
    // --- Insert merging ---
    expect_merge("contiguous inserts merge",
        {Kind::Insert, 0, "hello"}, {Kind::Insert, 5, " world"}, 100, false,
        {Kind::Insert, 0, "hello world"});

    expect_no_merge("a cursor jump (non-contiguous insert) does not merge",
        {Kind::Insert, 0, "hello"}, {Kind::Insert, 10, "x"}, 100, false);

    // --- Delete merging ---
    // Backspace-direction: repeatedly deleting the char just before the
    // cursor produces erase calls at decreasing offsets, each ending
    // exactly where the previous deletion's range started.
    expect_merge("backspace-direction deletes merge (prepend)",
        {Kind::Delete, 5, "o"}, {Kind::Delete, 4, "l"}, 100, false,
        {Kind::Delete, 4, "lo"});

    // Delete-key-direction: repeatedly deleting the char after the cursor
    // (cursor doesn't move) produces erase calls at the same offset.
    expect_merge("delete-key-direction deletes merge (append)",
        {Kind::Delete, 4, "l"}, {Kind::Delete, 4, "o"}, 100, false,
        {Kind::Delete, 4, "lo"});

    expect_no_merge("a delete at an unrelated offset does not merge",
        {Kind::Delete, 5, "o"}, {Kind::Delete, 0, "x"}, 100, false);

    // --- Kind mismatch ---
    expect_no_merge("an insert never merges with a preceding delete",
        {Kind::Delete, 5, "o"}, {Kind::Insert, 5, "x"}, 100, false);
    expect_no_merge("a delete never merges with a preceding insert",
        {Kind::Insert, 0, "hello"}, {Kind::Delete, 5, "o"}, 100, false);

    // --- Timing ---
    expect_no_merge("a 700ms+ gap forces a new record even when contiguous",
        {Kind::Insert, 0, "hello"}, {Kind::Insert, 5, " world"}, 701, false);
    expect_merge("just under the 700ms threshold still merges",
        {Kind::Insert, 0, "hello"}, {Kind::Insert, 5, " world"}, 699, false,
        {Kind::Insert, 0, "hello world"});

    // --- force_new_group ---
    expect_no_merge("force_new_group wins even for an otherwise-mergeable contiguous insert",
        {Kind::Insert, 0, "hello"}, {Kind::Insert, 5, " world"}, 100, true);

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All undo stack tests passed\n";
    return 0;
}
