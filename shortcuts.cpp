#include "shortcuts.h"

#include <cctype>
#include <map>
#include <set>

namespace {

// Key names GTK spells one way and people read another. Anything not here
// passes through unchanged, which covers F-keys, digits and letters.
const std::map<std::string, std::string>& key_names() {
    static const std::map<std::string, std::string> names = {
        {"Return", "Enter"},
        {"KP_Enter", "Keypad Enter"},
        {"KP_Add", "Keypad +"},
        {"KP_Subtract", "Keypad -"},
        {"Escape", "Esc"},
        {"plus", "+"},
        {"minus", "-"},
        {"equal", "="},
        {"underscore", "_"},
        {"ISO_Left_Tab", "Tab"},
    };
    return names;
}

} // namespace

const std::vector<ShortcutEntry>& shortcut_table() {
    // Order here is the order the Help window shows: the things this app
    // exists for first, then ordinary editing.
    static const std::vector<ShortcutEntry> table = {
        {"Running commands", "<Control>Return", "win.run",
         "Run the current line (or selection)"},
        // The keypad twin of the line above - registered, but listing it
        // would just repeat the row.
        {"Running commands", "<Control>KP_Enter", "win.run", nullptr},
        {"Running commands", "<Control><Shift>Return", nullptr,
         "Run it in a terminal window"},
        {"Running commands", "<Control>c", nullptr,
         "Stop the running command (otherwise: Copy)"},
        {"Running commands", "<Control>d", "win.send-eof",
         "Send EOF to the running command"},

        {"File", "<Control>o", "win.open", "Open a file"},
        {"File", "<Control>s", "win.save", "Save"},
        {"File", "<Control>q", "win.quit", "Quit"},

        {"Editing", "<Control>z", "win.undo", "Undo"},
        {"Editing", "<Control><Shift>z", "win.redo", "Redo"},
        {"Editing", "<Control>l", "win.select-line", "Select the current line"},
        {"Editing", "Tab", nullptr, "Indent the selected lines"},
        {"Editing", "<Shift>Tab", nullptr, "Dedent the selected lines"},
        {"Editing", "<Control><Shift>d", nullptr, "Duplicate the current line"},
        {"Editing", "<Alt>Up", nullptr, "Move the line or selection up"},
        {"Editing", "<Alt>Down", nullptr, "Move the line or selection down"},

        {"Search", "<Control>f", "win.find", "Find"},
        {"Search", "<Control>g", "win.find-next", "Find next"},
        {"Search", "<Control><Shift>g", "win.find-previous", "Find previous"},
        {"Search", "<Control>h", "win.replace", "Find and replace"},
        {"Search", "Escape", nullptr, "Close the search bar"},

        {"View", "<Control>plus", nullptr, "Zoom in"},
        {"View", "<Control>minus", nullptr, "Zoom out"},
        {"View", "<Control>0", nullptr, "Reset the zoom"},

        {"Help", "F1", "win.shortcuts", "Show this window"},
    };
    return table;
}

std::vector<std::string> shortcut_groups() {
    std::vector<std::string> groups;
    std::set<std::string> seen;
    for (const ShortcutEntry& entry : shortcut_table()) {
        if (seen.insert(entry.group).second) {
            groups.push_back(entry.group);
        }
    }
    return groups;
}

std::string duplicate_accelerator() {
    std::set<std::string> seen;
    for (const ShortcutEntry& entry : shortcut_table()) {
        if (!seen.insert(entry.accelerator).second) {
            return entry.accelerator;
        }
    }
    return "";
}

std::string accelerator_label(const std::string& accelerator) {
    if (accelerator.empty()) return "";

    // Modifiers are emitted in a fixed order rather than the order they were
    // written, so "<Shift><Control>x" and "<Control><Shift>x" read the same.
    bool ctrl = accelerator.find("<Control>") != std::string::npos ||
                accelerator.find("<Primary>") != std::string::npos;
    bool alt = accelerator.find("<Alt>") != std::string::npos;
    bool shift = accelerator.find("<Shift>") != std::string::npos;

    // Whatever follows the last '>' is the key itself.
    size_t key_start = accelerator.find_last_of('>');
    std::string key = (key_start == std::string::npos)
        ? accelerator
        : accelerator.substr(key_start + 1);

    auto named = key_names().find(key);
    if (named != key_names().end()) {
        key = named->second;
    } else if (key.size() == 1) {
        // A single letter is written lowercase in accelerator syntax but
        // always shown uppercase.
        key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    }

    std::string label;
    if (ctrl) label += "Ctrl+";
    if (alt) label += "Alt+";
    if (shift) label += "Shift+";
    label += key;
    return label;
}
