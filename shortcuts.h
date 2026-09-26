#ifndef SHORTCUTS_H
#define SHORTCUTS_H

#include <string>
#include <vector>

// The one list of keyboard bindings. It is used twice: ShellSheet's
// setup_actions() registers accelerators from it, and Help > Keyboard
// Shortcuts renders it. Keeping both off a single table is the point - a
// reference that is maintained separately from the bindings drifts out of
// date almost immediately.
//
// GTK-free on purpose, like directory_tracking and terminal_launch, so the
// invariants that actually bite (two commands claiming one key) can be
// unit-tested without a display.

struct ShortcutEntry {
    // Heading it appears under, e.g. "Running commands". Entries are grouped
    // in the order the groups first appear in the table.
    const char* group;
    // GTK accelerator syntax, e.g. "<Control>Return".
    const char* accelerator;
    // Action to register the accelerator against, e.g. "win.run". Null when
    // ShellSheet::on_key_pressed() handles the key itself - deliberately not
    // an accelerator, because an accelerator is matched before the focused
    // widget sees the key, which is what once stopped Ctrl+C from copying.
    const char* action;
    // Shown in the Help window. Null marks an alias that should be
    // registered but not listed, e.g. the keypad twin of an Enter binding,
    // which would otherwise just be a second row saying the same thing.
    const char* description;
};

const std::vector<ShortcutEntry>& shortcut_table();

// Group names in the order they first appear in the table, each once.
std::vector<std::string> shortcut_groups();

// The first accelerator that appears on more than one row, or "" if none
// does. Two commands sharing a key is the bug class this project keeps
// running into, so it is worth a test rather than a code review.
std::string duplicate_accelerator();

// Turns GTK accelerator syntax into something to show a person:
// "<Control>Return" -> "Ctrl+Enter", "<Control><Shift>z" -> "Ctrl+Shift+Z".
// Unknown key names are passed through unchanged.
std::string accelerator_label(const std::string& accelerator);

#endif // SHORTCUTS_H
