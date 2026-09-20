# Undo/Redo — Design

**Date:** 2026-09-16
**Status:** Approved for implementation planning

## Context

`shell_sheet` (a gtkmm3/C++17 text editor with an attached "run the
current line as a shell command" worksheet) has no undo/redo at all.
GTK3's `Gtk::TextBuffer` provides no built-in undo support — only
`begin_user_action()`/`end_user_action()`, a pair of marker signals with
no consumer in this codebase. Adding real undo/redo is a new subsystem:
the buffer is mutated from many places (interactive typing via
`TextView`'s internal handling, streamed command output, interactive
stdin line submission, file Open/New, Find & Replace's single-Replace
and Replace All, and all six Text-menu transforms), and a correct
design has to cover all of them without being instrumented at each
site individually.

This document was produced via `superpowers:brainstorming`
(architectural path). Three scope questions were put to the user and
answered before this design was finalized:

1. **Undo granularity**: grouped (typing coalesces into logical undo
   steps, like every mainstream editor) — chosen over one-record-per-
   keystroke.
2. **Command output undo-ability**: included — output insertions are
   undoable like any other edit, not excluded as "scrollback."
3. **Existing confirmation dialogs**: Replace All and the whole-buffer
   Text transforms currently show a "this cannot be undone" dialog
   (added specifically because there was no undo). Those dialogs are
   **removed** once real undo exists — their own premise becomes false,
   and Ctrl+Z is the consistent safety net for every edit including
   these.

## Goals

- Undo/redo that covers every way the buffer can change, including
  future call sites, without needing to remember to instrument them.
- Typing groups into sentence/word-scale undo steps, not one step per
  keystroke.
- Command output, Replace/Replace All, and Text transforms are all
  undoable.
- No crashes or corrupted state from undoing/redoing while a command is
  running, or from rapid undo/redo sequences.

## Non-goals (v1)

- Clearing the modified-title asterisk (`*`) when undo lands back on
  the last-saved buffer state. Real editors track a "clean index" into
  the undo stack for this; it's a legitimate follow-up, not required
  for v1. Flagged here so it isn't forgotten, not silently dropped.
- Any undo-stack size cap. Buffers in this app are small (shell
  scripts); unbounded is the simplest correct choice.
- Persisting undo history across app restarts, or across a file
  Open/New (loading a different file clears the undo/redo stacks —
  see "File load" below).

## Architecture

### `undo_stack.h`/`.cpp`: the pure, testable core

Following this codebase's established pattern (`syntax_highlight.h`,
`text_search.h`, `text_transforms.h` — GTK-free modules holding the
actual logic, included into `shell_sheet.h`), the record type and the
merge decision itself live in a new GTK-free module, not directly in
`shell_sheet.h`:

```cpp
// undo_stack.h
struct EditRecord {
    enum class Kind { Insert, Delete } kind;
    int offset;       // buffer character offset
    std::string text; // inserted text, or the text that was deleted
};

// Pure decision function, no Gtk::TextBuffer/TextIter involved: given the
// previous record on top of the undo stack and a freshly-captured new
// edit (plus milliseconds elapsed since the previous edit), does the new
// edit merge into `prev` in place, or does it need to start a new record?
// Returns true and mutates `prev` in place if it merges; returns false
// (leaving `prev` untouched) if the caller should push `new_record` as a
// new entry instead. `force_new_group` short-circuits straight to false.
bool try_merge_edit(EditRecord& prev, const EditRecord& new_record,
                     long elapsed_ms, bool force_new_group);
```

`ShellSheet` holds the stacks and guard state as members and calls into
`try_merge_edit()` to decide push-vs-merge:

```cpp
#include "undo_stack.h"
...
std::vector<EditRecord> m_undo_stack;
std::vector<EditRecord> m_redo_stack;
bool m_performing_undo_redo = false;
bool m_force_new_undo_group = false;
std::chrono::steady_clock::time_point m_last_edit_time;
```

**Byte vs. character offsets**: `Gtk::TextIter::get_offset()` is
character-based, but `EditRecord::text` is a `std::string`, whose
`.length()` is a *byte* count. Everywhere this design computes an end
offset from a start offset and a record's text (undoing an `Insert`,
merge-contiguity checks), it needs the text's *character* length, not
`.length()`. For ASCII shell-script content the two are identical, so
this can use `.length()` directly and stay consistent with this
codebase's existing, already-accepted simplification (`syntax_highlight.h`
and `text_search.h` both document the same byte-vs-character tradeoff
for the same reason: the content this app targets is overwhelmingly
ASCII). Multi-byte UTF-8 text would make undo's computed range land in
the wrong place. Worth a one-line code comment at each use, matching
how the existing modules flag it, rather than building real UTF-8-aware
offset conversion nobody asked for.

Connected once, in the constructor, alongside the buffer's existing
`signal_changed()` hookup:

```cpp
m_text_buffer->signal_insert().connect(
    sigc::mem_fun(*this, &ShellSheet::on_buffer_insert), false /* before default handler */);
m_text_buffer->signal_erase().connect(
    sigc::mem_fun(*this, &ShellSheet::on_buffer_erase), false /* before default handler */);
```

Both `signal_insert()` and `signal_erase()` fire for **every**
mutation of the buffer regardless of source — interactive typing
(handled internally by `Gtk::TextView`, never visible as a call site
in this codebase), `paste_clipboard()`/`cut_clipboard()`, and every one
of our own `insert()`/`erase()` calls. Connected with `after = false`
(the default), our handlers run *before* GTK's own default handler
actually performs the edit — critical for `on_buffer_erase`, which
must call `get_text(start, end)` to capture the doomed text while it
still exists.

```cpp
void ShellSheet::on_buffer_insert(const Gtk::TextBuffer::iterator& pos,
                                   const Glib::ustring& text, int /*bytes*/) {
    if (m_performing_undo_redo) return;
    record_edit(EditRecord::Kind::Insert, pos.get_offset(), text);
}

void ShellSheet::on_buffer_erase(const Gtk::TextBuffer::iterator& start,
                                  const Gtk::TextBuffer::iterator& end) {
    if (m_performing_undo_redo) return;
    record_edit(EditRecord::Kind::Delete, start.get_offset(), m_text_buffer->get_text(start, end));
}
```

### Guarding against self-recording during undo/redo

`undo()` and `redo()` apply the inverse of a stack entry by calling
`erase()`/`insert()` on the buffer — which would otherwise re-trigger
the same two signals and record a bogus new entry, corrupting both
stacks. `m_performing_undo_redo` is set `true` for the duration of that
one mutation and checked first in both handlers above. `undo()`/
`redo()` push the correct inverse record onto the *other* stack
themselves, rather than relying on the generic capture for their own
mutation:

```cpp
void ShellSheet::undo() {
    if (m_undo_stack.empty()) return;
    EditRecord record = m_undo_stack.back();
    m_undo_stack.pop_back();

    m_performing_undo_redo = true;
    if (record.kind == EditRecord::Kind::Insert) {
        auto start = m_text_buffer->get_iter_at_offset(record.offset);
        auto end = m_text_buffer->get_iter_at_offset(record.offset + record.text.length());
        m_text_buffer->erase(start, end);
        m_text_buffer->place_cursor(m_text_buffer->get_iter_at_offset(record.offset));
    } else {
        auto pos = m_text_buffer->get_iter_at_offset(record.offset);
        m_text_buffer->insert(pos, record.text);
        m_text_buffer->place_cursor(m_text_buffer->get_iter_at_offset(record.offset + record.text.length()));
    }
    m_performing_undo_redo = false;

    m_redo_stack.push_back(record); // inverse of undo is redo: same record, opposite stack
    update_undo_redo_sensitivity();
}
```

`redo()` is the mirror: pop from `m_redo_stack`, re-apply the record
as originally recorded (re-insert an `Insert`, re-delete a `Delete`),
push back onto `m_undo_stack`.

Any **new** edit (an insert/erase whose signal fires with
`m_performing_undo_redo == false`) clears `m_redo_stack` — standard
behavior; you can't redo past a point where you've since made a new
change.

### Marks

`m_command_mark` and `m_input_mark` (the existing marks driving output
placement and interactive-stdin input boundaries) need no undo-specific
handling — GTK repositions `Gtk::TextMark`s automatically across any
insert/erase per their existing gravity rules, regardless of whether
the mutation came from typing, output, or an undo/redo operation.

## Grouping algorithm

GTK's `TextView` handles real keystrokes and Backspace/Delete-key
presses internally; there is no call site in this codebase for "a
letter was typed." So grouping is inferred from the edit stream
itself, with one piece of explicit help from our own mutation sites.

`ShellSheet::record_edit()` gathers the current context (elapsed time
since `m_last_edit_time`, the `m_force_new_undo_group` flag) and calls
the pure `try_merge_edit()` from `undo_stack.h` against
`m_undo_stack.back()` (call it `prev`) when the stack is non-empty.
The decision procedure `try_merge_edit()` implements:

1. If `m_force_new_undo_group` is set: never merge. Push a new record,
   clear the flag.
2. Otherwise, if `prev.kind != kind`: never merge (an insert never
   merges with a preceding delete or vice versa).
3. Otherwise, if more than **700ms** have elapsed since
   `m_last_edit_time`: never merge (a paused-then-resumed typing burst
   is two undo steps, not one).
4. Otherwise, contiguity check depends on `kind`:
   - **Insert**: merges if the new edit's `offset` equals
     `prev.offset + prev.text.length()` (typing landed exactly where
     the previous insert ended) → `prev.text += new_text`.
   - **Delete**: merges in either direction, matching how Backspace and
     the Delete key each behave:
     - Backspace-style (deleting backward, cursor moves left): new
       edit's range ends exactly at `prev.offset` → prepend:
       `prev.offset = new_offset; prev.text = new_text + prev.text;`
     - Delete-key-style (deleting forward, cursor doesn't move): new
       edit's `offset` equals `prev.offset` → append:
       `prev.text += new_text;`
5. If none of the above merge conditions hold: push a new record.

`m_last_edit_time` is updated to "now" on every recorded edit
(merged or not).

### `m_force_new_undo_group`: which call sites set it

Every one of *our own* direct buffer-mutating call sites sets this
flag to `true` immediately before mutating, guaranteeing that
operation is always its own standalone undo step — never silently
absorbed into adjacent typing, and never absorbing adjacent typing
into itself:

- `flush_output_buffer()` — one flush (already batched by the existing
  100ms output timer) = one undo step. No new batching logic needed;
  this falls out of the existing flush cadence for free.
- `apply_text_transform()` — one transform invocation = one undo step,
  whether it touched a selection or the whole buffer.
- `on_replace_current()` and `on_replace_all()` — ensures the first
  substitution of either doesn't merge with whatever edit preceded it
  (see "Replace All" below for why this does *not* mean Replace All's
  many substitutions collapse into one undo step).
- The stdin-echo insert in `on_key_press()` — submitting a line to a
  running command's stdin is one undo step.
- File Open/New (`on_menu_open()`, `on_menu_new()`) — see "File load"
  below; in practice these clear the stacks entirely rather than
  relying on this flag.
- `on_menu_paste()` — see "Paste is asynchronous" below.

### Paste is asynchronous

`Gtk::TextBuffer::paste_clipboard()` does not insert synchronously —
the actual `insert()` (and thus `signal_insert()`) fires later, when
the clipboard contents are received, via GDK's normal event queue.
Setting `m_force_new_undo_group = true` immediately before calling
`paste_clipboard()` is still correct: the flag is consumed by the
*next* insert/erase signal to fire, whenever that happens, and no
other buffer mutation can realistically occur in the sub-frame gap
before the paste completes.

### Replace All

Replace All performs multiple `erase()`/`insert()` pairs in one call
(iterating matches in reverse, per the existing implementation). Each
of those would naturally be its own `EditRecord` under the grouping
rules above (each substitution is a Delete immediately followed by an
Insert at the same point — different `kind`, so they'd never merge
with each other anyway). Setting `m_force_new_undo_group` once, before
the loop begins, only guarantees the *first* substitution in the loop
doesn't merge with whatever preceded Replace All — it does **not**
collapse all N substitutions into a single undo step.

**Decision**: leave Replace All as N undo steps (one Delete+Insert
pair per replaced occurrence), not one. Rationale: correctly collapsing
an arbitrary multi-substitution operation into a single undo record
would mean recording it as one big composite edit rather than reusing
the same simple `EditRecord` shape everything else uses — real scope
increase for a cosmetic difference (Ctrl+Z a few extra times vs. once)
that doesn't block "Replace All is undoable" from being true. Worth
revisiting later if it's annoying in practice.

## Cursor/selection after undo/redo

Per the code above: after undoing an `Insert` (erasing it), the cursor
lands at the erase point. After undoing a `Delete` (re-inserting the
text), the cursor lands at the end of the re-inserted text. No
selection is set. Simple, consistent, and matches how many editors
actually behave (not all editors select the restored range).

## File load clears undo history

`on_menu_new()` and `on_menu_open()` call `m_text_buffer->set_text()`
to replace the entire buffer. Rather than making that replacement
itself one giant undoable edit (which would let you "undo" back into a
totally different file's content — confusing and not how most editors
behave), both handlers clear `m_undo_stack` and `m_redo_stack`
outright after loading. Loading a file starts a fresh undo history.

## Removed: "cannot be undone" confirmation dialogs

Per the scope decision above, the `Gtk::MessageDialog` confirmations in
`on_replace_all()` (Replace All) and `apply_text_transform()`
(whole-buffer transforms, no selection active) are removed entirely.
Both operations proceed immediately, exactly like every other edit —
undo is now the actual safety net.

## UI

Edit menu gains two items at the **top**, above Cut/Copy/Paste/Select
All/Select Line (standard editor menu ordering):

- **Undo** — Ctrl+Z
- **Redo** — Ctrl+Shift+Z (no existing accelerator collision; `z` and
  `y` are both currently free, `Ctrl+Shift+Z` chosen to match
  GNOME/gedit convention on this platform)

Both start disabled (`set_sensitive(false)`) and get their sensitivity
updated by a new `update_undo_redo_sensitivity()` helper — called after
every push/pop of either stack (i.e., from `record_edit()`, `undo()`,
`redo()`, and the file-load clear) — mirroring the existing pattern
used for Find & Replace's Previous/Next/Replace button sensitivity.

## Testing

Same two-tier split as every other feature built this session:

- **Pure logic → GTK-free unit tests** (`make test`): a new
  `undo_stack.h/.cpp` holding the merge-decision function in isolation
  — given a sequence of `(kind, offset, text, elapsed_ms)` tuples (no
  `Gtk::TextIter`/buffer involved at all), does it merge into the
  previous record or start a new one? Covers: contiguous inserts
  merge; a cursor jump doesn't; Backspace-direction delete merging;
  Delete-key-direction delete merging; a 700ms+ gap forces a new
  record even when positionally contiguous; `Kind` mismatch never
  merges; the forced-new-group flag always wins regardless of
  contiguity/timing.
- **GTK-coupled behavior → temporary test hooks + a real Xvfb smoke
  test**, removed after verification, per this session's established
  pattern: typing a burst of characters and confirming one Ctrl+Z-
  equivalent (`undo()` call) removes the whole burst; a paste being a
  standalone step; Replace All producing the expected number of undo
  steps; undo/redo correctly round-tripping through several
  operations in sequence (undo, undo, redo, new edit clears redo
  stack); file Open clearing both stacks; no crash from calling
  `undo()`/`redo()` when the respective stack is empty.

## Files touched

- `undo_stack.h`, `undo_stack.cpp` (new) — pure merge-decision logic
  and the `EditRecord` type, mirroring the existing
  `syntax_highlight.h`/`text_search.h`/`text_transforms.h` pattern of
  GTK-free modules with their own unit tests.
- `tests/test_undo_stack.cpp` (new).
- `shell_sheet.h` — new members (`m_undo_stack`, `m_redo_stack`, the
  two guard flags, `m_last_edit_time`), new menu item members
  (`m_item_undo`, `m_item_redo`), new methods (`on_buffer_insert`,
  `on_buffer_erase`, `record_edit`, `undo`, `redo`,
  `update_undo_redo_sensitivity`, `on_menu_undo`, `on_menu_redo`).
- `shell_sheet.cpp` — signal wiring in the constructor; the Edit menu
  additions in `setup_menu()`/`setup_shortcuts()`; `m_force_new_undo_group`
  set at each of the call sites listed above; the stack-clearing calls
  in `on_menu_new()`/`on_menu_open()`; removal of the two confirmation
  dialogs.
- `Makefile` — add `undo_stack.cpp` to `SRCS`; add its test binary to
  the `test` target (GTK-free, same build line style as
  `test_text_transforms`).

## Open follow-ups (not in this pass)

- Clearing the modified-title asterisk when undo returns to the
  last-saved state (needs a tracked "clean index" into the undo
  stack).
- Whether Replace All should eventually collapse to a single undo
  step instead of N.
