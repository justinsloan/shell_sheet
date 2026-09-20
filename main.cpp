#include "shell_sheet.h"
#include <gtkmm.h>
#include <glib-unix.h>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// Invoked instead of the normal editor when SHELL_SHEET_ASKPASS is set in
// the environment. shell_sheet.cpp's on_menu_terminal() sets that (plus
// SUDO_ASKPASS pointing back at this same binary) on any command it runs
// through sudo, so sudo calls back into us just for the password prompt.
// The contract for an askpass helper is simple: print the entered password
// to stdout, exit 0; print nothing and exit non-zero to cancel.
int run_sudo_askpass() {
    // sudo invokes an askpass helper as `helper_path "<prompt text>"`, e.g.
    // helper_path "[sudo] password for justin: ". We don't use that prompt
    // (we show our own fixed dialog), and deliberately don't forward it as
    // argv here: GApplication treats a stray positional argument as a file
    // to open, and since this app never declares that it handles opening
    // files, that fires a "This application can not open files" critical
    // and activate() never runs - so the dialog never appears, and sudo
    // sees no password at all ("Authentication required but not
    // attempted"). Passing no arguments sidesteps that entirely.
    int argc = 0;
    char** argv = nullptr;
    // NON_UNIQUE: each invocation must show and answer its own prompt
    // independently, never be forwarded to some other already-running
    // instance of this helper (or of the main editor).
    auto app = Gtk::Application::create(argc, argv, "com.justin.shellsheet.askpass",
                                         Gio::APPLICATION_NON_UNIQUE);
    int exit_code = 1;

    app->signal_activate().connect([&]() {
        Gtk::Dialog dialog("Sudo Authentication Required", true);
        Gtk::Entry password_entry;
        password_entry.set_visibility(false);
        password_entry.set_placeholder_text("Enter sudo password");
        // Enter submits, the way every other password prompt behaves.
        // Both halves are needed: set_activates_default makes Enter in the
        // entry activate the dialog's default widget, and
        // set_default_response is what makes OK that widget.
        password_entry.set_activates_default(true);
        dialog.get_content_area()->pack_start(password_entry, true, true, 10);
        dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("_OK", Gtk::RESPONSE_OK);
        dialog.set_default_response(Gtk::RESPONSE_OK);
        dialog.show_all_children();
        // Type straight into the entry without clicking it first.
        password_entry.grab_focus();

        if (dialog.run() == Gtk::RESPONSE_OK) {
            std::string password = password_entry.get_text();
            std::cout << password << std::endl;
            std::fill(password.begin(), password.end(), '\0');
            exit_code = 0;
        }
        app->quit();
    });

    app->run();
    return exit_code;
}

// Handles SIGTERM/SIGINT via GLib's main-loop-integrated signal handling
// (g_unix_signal_add's callback runs as a normal main-loop source, so unlike
// a raw signal() handler it's safe to call arbitrary code here). Sends
// SIGTERM to any currently-running command so it isn't left orphaned when
// the app closes, then closes the window the same way File > Quit does.
//
// This can never catch SIGKILL ("kill -9"): that signal is intentionally
// uncatchable by any process, by design. A command killed that way is
// unavoidably left running - nothing in userspace can prevent that, only
// something like a process supervisor or the desktop session cleaning up
// afterward.
gboolean handle_termination_signal(gpointer user_data) {
    auto* window = static_cast<ShellSheet*>(user_data);
    window->terminate_running_command();
    window->hide();
    return G_SOURCE_REMOVE;
}

} // namespace

int main(int argc, char* argv[]) {
    // Writing a typed line to a running command's stdin (see
    // ShellSheet::on_key_press) can hit a closed pipe if that command isn't
    // reading stdin, or already exited. Without this, that write raises
    // SIGPIPE and kills the whole app; with it, the write just fails and we
    // ignore the failure.
    std::signal(SIGPIPE, SIG_IGN);

    if (std::getenv("SHELL_SHEET_ASKPASS")) {
        return run_sudo_askpass();
    }

    auto app = Gtk::Application::create(argc, argv, "com.justin.shellsheet");
    ShellSheet window;
    g_unix_signal_add(SIGTERM, handle_termination_signal, &window);
    g_unix_signal_add(SIGINT, handle_termination_signal, &window);
    return app->run(window);
}
