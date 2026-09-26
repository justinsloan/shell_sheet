// Standalone, dependency-free tests for the keyboard-shortcut table.
// Run via `make test`.
#include "../shortcuts.h"

#include <iostream>
#include <set>
#include <string>

namespace {

int g_failures = 0;

void expect_eq(const std::string& label, const std::string& actual,
                const std::string& expected) {
    if (actual == expected) {
        std::cout << "PASS: " << label << "\n";
        return;
    }
    ++g_failures;
    std::cout << "FAIL: " << label << "\n  expected: [" << expected
              << "]\n  actual:   [" << actual << "]\n";
}

void expect_true(const std::string& label, bool cond) {
    std::cout << (cond ? "PASS: " : "FAIL: ") << label << "\n";
    if (!cond) ++g_failures;
}

} // namespace

int main() {
    const auto& table = shortcut_table();

    expect_true("the table is not empty", !table.empty());

    // --- the invariant that actually bites ---
    expect_eq("no accelerator is claimed by two commands", duplicate_accelerator(), "");

    // --- every row is well formed ---
    bool all_have_group = true, all_have_accel = true;
    bool aliases_have_an_action = true, actions_are_namespaced = true;
    for (const ShortcutEntry& e : table) {
        if (!e.group || !*e.group) all_have_group = false;
        if (!e.accelerator || !*e.accelerator) all_have_accel = false;
        // A row with no description is an alias; it must be registering
        // something, or it is just an accelerator that does nothing.
        if (!e.description && !e.action) aliases_have_an_action = false;
        if (e.action && std::string(e.action).rfind("win.", 0) != 0) {
            actions_are_namespaced = false;
        }
    }
    expect_true("every row names a group", all_have_group);
    expect_true("every row has an accelerator", all_have_accel);
    expect_true("an undescribed row is registering an action", aliases_have_an_action);
    expect_true("every action is window-scoped (win.*)", actions_are_namespaced);

    // --- groups: each once, in first-appearance order ---
    std::vector<std::string> groups = shortcut_groups();
    std::set<std::string> unique(groups.begin(), groups.end());
    expect_true("groups are listed without repeats", unique.size() == groups.size());

    std::vector<std::string> first_seen;
    std::set<std::string> seen;
    for (const ShortcutEntry& e : table) {
        if (seen.insert(e.group).second) first_seen.push_back(e.group);
    }
    expect_true("groups come back in first-appearance order", groups == first_seen);
    expect_true("every group in the table is listed", groups.size() == first_seen.size());

    // --- the bindings a user is most likely to look up are present ---
    auto has = [&](const std::string& accel) {
        for (const ShortcutEntry& e : table) {
            if (accel == e.accelerator) return true;
        }
        return false;
    };
    expect_true("Ctrl+Enter (run the current line) is listed", has("<Control>Return"));
    expect_true("its keypad twin is registered too", has("<Control>KP_Enter"));
    expect_true("Ctrl+Shift+Enter (run in a terminal) is listed",
                has("<Control><Shift>Return"));
    expect_true("Ctrl+C (terminate) is listed", has("<Control>c"));
    expect_true("Tab (indent) is listed", has("Tab"));
    expect_true("F1 (this very window) is listed", has("F1"));

    // Ctrl+C must NOT be registered as an accelerator: it has to keep
    // meaning Copy while nothing is running.
    for (const ShortcutEntry& e : table) {
        if (std::string(e.accelerator) == "<Control>c") {
            expect_true("Ctrl+C is documented but NOT registered as an accelerator",
                        e.action == nullptr);
        }
        if (std::string(e.accelerator) == "Tab") {
            expect_true("Tab is documented but NOT registered as an accelerator",
                        e.action == nullptr);
        }
    }

    // --- accelerator_label ---
    expect_eq("plain key", accelerator_label("F1"), "F1");
    expect_eq("control plus a letter", accelerator_label("<Control>o"), "Ctrl+O");
    expect_eq("control and shift", accelerator_label("<Control><Shift>z"), "Ctrl+Shift+Z");
    expect_eq("Return reads as Enter", accelerator_label("<Control>Return"), "Ctrl+Enter");
    expect_eq("keypad Enter is spelled out",
              accelerator_label("<Control>KP_Enter"), "Ctrl+Keypad Enter");
    expect_eq("alt and an arrow", accelerator_label("<Alt>Up"), "Alt+Up");
    expect_eq("symbol keys read as symbols", accelerator_label("<Control>plus"), "Ctrl++");
    expect_eq("minus too", accelerator_label("<Control>minus"), "Ctrl+-");
    expect_eq("Escape is abbreviated", accelerator_label("Escape"), "Esc");
    expect_eq("shift-tab", accelerator_label("<Shift>Tab"), "Shift+Tab");
    expect_eq("digits pass through", accelerator_label("<Control>0"), "Ctrl+0");
    expect_eq("an unknown key name passes through",
              accelerator_label("<Control>Bogus"), "Ctrl+Bogus");
    expect_eq("modifier order is Ctrl, Alt, Shift",
              accelerator_label("<Shift><Alt><Control>x"), "Ctrl+Alt+Shift+X");
    expect_eq("an empty accelerator yields nothing", accelerator_label(""), "");

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All shortcut table tests passed\n";
    return 0;
}
