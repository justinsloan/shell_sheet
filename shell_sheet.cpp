#include "shell_sheet.h"
#include "syntax_highlight.h"
#include <gtkmm.h>
#include <glibmm.h>
#include <iostream>
#include <deque>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <cstring> // strsignal
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <algorithm>

using namespace Gtk;

const std::string BUILD_NUMBER = "10";

// One indent level, in spaces. Used by both the Text menu's Indent/Dedent
// items and the Tab/Shift+Tab handling in on_key_press(), so the two can
// never disagree about how far a level is.
const int kIndentWidth = 4;

// Horizontal breathing room on each side of the line numbers in the gutter.
const int kGutterPadding = 5;

// How many lines of a multi-line command the running indicator shows
// before replacing the rest with a "..." line.
const int kRunningBarMaxLines = 3;

// Column that Text > Hard Wrap breaks at. 80 is the conventional terminal
// width, which is what most of this buffer's text came out of.
const int kHardWrapWidth = 80;

ShellSheet::ShellSheet() {
    set_title("Shell Sheet");
    set_default_size(800, 600);

    apply_css();
    setup_ui_layout();
    setup_menu();
    setup_shortcuts();

    m_text_buffer = m_text_view.get_buffer();
    m_command_mark = m_text_buffer->create_mark("command_mark", m_text_buffer->end(), false);
    // Left gravity: unlike m_command_mark, this must NOT get dragged forward
    // by the very insertions it's supposed to bound (the user's own typed
    // characters land exactly at its position), so it stays put while text
    // accumulates after it. See on_key_press().
    m_input_mark = m_text_buffer->create_mark("input_mark", m_text_buffer->end(), true);
    m_text_buffer->signal_changed().connect(sigc::mem_fun(*this, &ShellSheet::on_text_buffer_changed));

    // Undo/Redo capture: these fire for every buffer mutation regardless of
    // source (typing, paste, our own insert()/erase() calls), and - run
    // before GTK's own default handler (the default `false` for the second
    // argument) - so on_buffer_erase can read the doomed text before it's
    // actually gone. See docs/superpowers/specs/2026-09-16-undo-redo-design.md.
    m_text_buffer->signal_insert().connect(sigc::mem_fun(*this, &ShellSheet::on_buffer_insert), false);
    m_text_buffer->signal_erase().connect(sigc::mem_fun(*this, &ShellSheet::on_buffer_erase), false);

    // Lets Enter forward a line of typed text to a running command's stdin
    // (see on_key_press). Connected before the default handler so we can
    // consume the event and suppress the normal newline-insertion behavior.
    m_text_view.signal_key_press_event().connect(
        sigc::mem_fun(*this, &ShellSheet::on_key_press), false);

    // Line number area drawing signal
    m_line_number_area.signal_draw().connect(sigc::mem_fun(*this, &ShellSheet::on_line_number_area_draw));
    
    // Connect scroll adjustment to redraw line numbers
    m_scrolled_window.get_vadjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &ShellSheet::on_scroll_changed)
    );

    setup_search_bar();

    // Commands start out where the app itself was launched from.
    char cwd[PATH_MAX];
    m_working_dir = (::getcwd(cwd, sizeof(cwd)) != nullptr) ? normalize_path(cwd) : "/";
    setup_cwd_bar();
    setup_running_bar();

    // Start with the caret in the editor. Without this the first focusable
    // widget wins, which is the selectable working-directory label - and a
    // GtkLabel selects its whole text when it takes focus, so the app
    // opened with the path highlighted as though the user had chosen it.
    m_text_view.grab_focus();

    // Last: needs both the buffer (to count lines) and the line-number area
    // (to measure them in the chosen font).
    apply_font_size();
}

ShellSheet::~ShellSheet() {
    // The font provider is registered on the screen, which outlives this
    // window, so it has to be taken off again explicitly - otherwise its
    // rules would keep applying to anything created afterwards.
    if (m_font_provider) {
        Gtk::StyleContext::remove_provider_for_screen(Gdk::Screen::get_default(),
                                                       m_font_provider);
    }
    if (m_running_css_provider) {
        Gtk::StyleContext::remove_provider_for_screen(Gdk::Screen::get_default(),
                                                       m_running_css_provider);
    }

    if (m_is_command_running && m_current_pid > 0) {
        kill(m_current_pid, SIGTERM);
    }

    // Close the command's pipe fds rather than relying on process exit to
    // reclaim them. Today this window lives for the whole run so it makes
    // no practical difference, but leaving fds dangling in a destructor is
    // the kind of thing that quietly becomes a real leak the moment the
    // window stops being a singleton.
    if (m_command_output_fd >= 0) {
        ::close(m_command_output_fd);
        m_command_output_fd = -1;
    }
    if (m_command_stdin_fd >= 0) {
        ::close(m_command_stdin_fd);
        m_command_stdin_fd = -1;
    }

    // The io/child watches auto-disconnect via sigc::trackable, but the
    // pending output-flush timeout is worth dropping explicitly so it can't
    // fire against a half-destroyed window.
    if (m_output_timeout_conn.connected()) {
        m_output_timeout_conn.disconnect();
    }
}

void ShellSheet::apply_css() {
    auto css = CssProvider::create();
    css->load_from_data(
        "window { margin: 0; padding: 0; border: none; }"
        "box { margin: 0; padding: 0; border: none; }"
        "menubar { margin: 0; padding: 0; border: none; }"
        "paned { margin: 0; padding: 0; border: none; }"
        "scrolledwindow { margin: 0; padding: 0; border: none; }"
        "textview { margin: 0; padding: 0; border: none; }"
        "drawingarea { margin: 0; padding: 0; border: none; }"
    );
    get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // The editor font lives in its own provider, on the TextView alone,
    // because unlike everything above it is reloaded at runtime every time
    // the zoom changes. Keeping it separate means a zoom never re-parses
    // the layout rules, and the layout rules can never be lost by a zoom.
    // Only registered here - actually loading a size is deferred to the
    // constructor, because apply_font_size() re-measures the gutter and
    // that needs m_text_buffer, which does not exist yet at this point.
    //
    // Registered for the whole screen rather than on the TextView's own
    // style context. That is GTK's supported scope for a provider, and it
    // matters here specifically: a widget-scoped provider is picked up when
    // the widget is realized but does not reliably re-invalidate a
    // GtkTextView's text layout afterwards, which is exactly what zooming
    // needs it to do.
    m_font_provider = CssProvider::create();
    Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), m_font_provider,
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

Pango::FontDescription ShellSheet::text_view_font() const {
    return m_text_view.get_style_context()->get_font(Gtk::STATE_FLAG_NORMAL);
}

void ShellSheet::apply_font_size() {
    // "monospace" is the CSS generic family: it resolves to whatever font
    // the system is configured to use for monospaced text, rather than
    // pinning a specific face that might not be installed.
    m_font_provider->load_from_data(
        "textview { font-family: monospace; font-size: " +
        std::to_string(m_font_size) + "pt; }");

    update_line_number_width(/*force_remeasure=*/true);
    m_line_number_area.queue_draw();

    // The TextView revalidates its text layout asynchronously, so the
    // redraw above can still run against the old line geometry. Queue a
    // second one below GTK's text-validation idle priority, so the gutter
    // repaints once the new line positions exist. (The numbers can't
    // actually drift out of step with the text - both read the same
    // GtkTextLayout - but without this the gutter could sit one frame
    // behind after a zoom.)
    Glib::signal_idle().connect_once(
        sigc::mem_fun(*this, &ShellSheet::redraw_line_numbers), Glib::PRIORITY_LOW);
}

void ShellSheet::redraw_line_numbers() {
    m_line_number_area.queue_draw();
}

void ShellSheet::set_font_size(int points) {
    int clamped = std::max(kMinFontSize, std::min(kMaxFontSize, points));
    if (clamped == m_font_size) return;
    m_font_size = clamped;
    apply_font_size();
}

void ShellSheet::setup_cwd_bar() {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
    row->set_margin_top(4);
    row->set_margin_bottom(4);
    row->set_margin_start(6);
    row->set_margin_end(6);

    auto* caption = Gtk::make_managed<Gtk::Label>();
    caption->set_markup("<b>pwd</b>");

    // Selectable so the path can be copied out; ellipsized at the START so
    // that when a deep path doesn't fit it's the leading directories that
    // get cut, leaving the part you actually need to see.
    m_cwd_label.set_selectable(true);
    m_cwd_label.set_ellipsize(Pango::ELLIPSIZE_START);
    m_cwd_label.set_xalign(0.0f);

    row->pack_start(*caption, false, false);
    row->pack_start(m_cwd_label, true, true);

    m_cwd_revealer.add(*row);
    m_cwd_revealer.set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    m_cwd_revealer.show_all();
    m_cwd_revealer.set_reveal_child(true);

    update_working_directory_display();
}

void ShellSheet::setup_running_bar() {
    // No margins here, deliberately: a margin is space OUTSIDE the widget,
    // so it would leave an unpainted border around the orange. The same
    // spacing is applied as CSS padding below, which is inside the widget
    // and therefore gets the background colour.
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);

    m_running_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_running_label.set_xalign(0.0f);
    row->pack_start(m_running_label, true, true);

    // Colour and weight live in CSS rather than Pango markup so the label's
    // text stays plain - it holds a command line, and markup would treat
    // any '<' or '&' in it as broken markup.
    row->get_style_context()->add_class("command-running-bar");

    m_running_css_provider = CssProvider::create();
    m_running_css_provider->load_from_data(
        ".command-running-bar { background-color: #f5a623; padding: 4px 6px; }"
        ".command-running-bar label { color: #ffffff; font-weight: bold; }");
    // Screen-scoped for the same reason the font provider is: a
    // widget-scoped provider is unreliable about actually taking effect.
    Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(),
                                                m_running_css_provider,
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    m_running_revealer.add(*row);
    m_running_revealer.set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    m_running_revealer.show_all();
    m_running_revealer.set_reveal_child(false); // nothing is running at startup
}

void ShellSheet::update_running_indicator() {
    if (m_is_command_running) {
        // A multi-line command (run from a selection) is cut to its first
        // three lines with a "..." line standing in for the rest, so the
        // bar can't grow to swallow the window.
        m_running_label.set_text("Running: " + clamp_lines(m_running_command, kRunningBarMaxLines));
        m_running_revealer.set_reveal_child(true);
    } else {
        m_running_revealer.set_reveal_child(false);
        m_running_command.clear();
    }
}

void ShellSheet::update_working_directory_display() {
    m_cwd_label.set_text(m_working_dir);
    m_cwd_label.set_tooltip_text(m_working_dir); // the full path, even when ellipsized
}

void ShellSheet::on_menu_toggle_cwd_bar() {
    m_cwd_revealer.set_reveal_child(m_item_show_cwd->get_active());
}

void ShellSheet::on_menu_wrap_mode_changed() {
    // WRAP_WORD_CHAR rather than WRAP_WORD: command output is full of long
    // unbroken tokens (paths, URLs, hashes) with no space to break at, and
    // WRAP_WORD would let those run off the edge anyway.
    m_text_view.set_wrap_mode(m_item_soft_wrap->get_active() ? Gtk::WRAP_WORD_CHAR
                                                              : Gtk::WRAP_NONE);
    // Wrapped lines occupy more vertical space, so every line's y position
    // moves; the gutter has to be redrawn against the new geometry.
    m_line_number_area.queue_draw();
}

void ShellSheet::run_cd_command(const std::string& command, const Gtk::TextIter& line_end) {
    // Place the response exactly where a real command's output would go.
    m_text_buffer->move_mark(m_command_mark, line_end);
    m_text_buffer->insert(m_command_mark->get_iter(), "\n");
    m_text_buffer->move_mark(m_input_mark, m_command_mark->get_iter());

    const char* home = std::getenv("HOME");
    std::string target = resolve_cd_target(m_working_dir, cd_argument(command),
                                            home ? home : "/", m_previous_working_dir);

    // Report failures the way a shell does, rather than silently staying
    // put - the working-directory bar may well be hidden.
    struct stat info;
    if (target.empty()) {
        append_status_line("cd: OLDPWD not set");
    } else if (::stat(target.c_str(), &info) != 0) {
        append_status_line("cd: " + target + ": No such file or directory");
    } else if (!S_ISDIR(info.st_mode)) {
        append_status_line("cd: " + target + ": Not a directory");
    } else if (::access(target.c_str(), X_OK) != 0) {
        append_status_line("cd: " + target + ": Permission denied");
    } else {
        m_previous_working_dir = m_working_dir;
        m_working_dir = target;
        update_working_directory_display();
        // Echo the new directory. A real shell prints nothing here, but in
        // a worksheet the transcript is the record of what happened, and
        // this is the only trace a cd leaves.
        append_status_line(m_working_dir);
    }

    m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
}

void ShellSheet::on_menu_zoom_in() { set_font_size(m_font_size + 1); }
void ShellSheet::on_menu_zoom_out() { set_font_size(m_font_size - 1); }
void ShellSheet::on_menu_zoom_reset() { set_font_size(kDefaultFontSize); }

void ShellSheet::update_line_number_width(bool force_remeasure) {
    // Sized to the largest number the gutter will actually draw. That is
    // get_line_count(), which includes the empty line a trailing newline
    // creates - that line is numbered too, so it has to fit.
    int digits = static_cast<int>(std::to_string(std::max(m_text_buffer->get_line_count(), 1)).size());
    if (digits == m_line_number_digits && !force_remeasure) return;
    m_line_number_digits = digits;

    // Measure a real string in the real font rather than guessing a
    // per-character width - at a large size the difference is many pixels,
    // and too narrow a gutter silently clips the digits.
    auto layout = m_line_number_area.create_pango_layout(std::string(digits, '0'));
    layout->set_font_description(text_view_font());
    int text_width = 0, text_height = 0;
    layout->get_pixel_size(text_width, text_height);

    int width = text_width + 2 * kGutterPadding;
    m_line_number_area.set_size_request(width, -1);
    // Always moved, in both directions: the gutter tracks the digit count,
    // so deleting a document down from 200 lines to 10 narrows it back to
    // two digits rather than leaving it stuck at its widest.
    m_paned.set_position(width);
}

void ShellSheet::setup_ui_layout() {
    add(m_vbox);
    m_vbox.set_spacing(0);
    m_vbox.set_margin_top(0);
    m_vbox.set_margin_bottom(0);
    m_vbox.set_margin_start(0);
    m_vbox.set_margin_end(0);

    // Pack menu bar at the TOP
    m_vbox.pack_start(m_menu_bar, PACK_SHRINK);
    m_menu_bar.set_margin_bottom(0);
    m_menu_bar.set_margin_top(0);

    // Working-directory bar, directly under the menu bar and above the
    // search bar. Contents are built in setup_cwd_bar().
    m_vbox.pack_start(m_cwd_revealer, PACK_SHRINK);

    // Running indicator, below the working-directory bar. Revealed only
    // while a command is in flight (see update_running_indicator()).
    m_vbox.pack_start(m_running_revealer, PACK_SHRINK);

    // Search bar sits right below the menu bar, hidden until Ctrl+F/H or
    // the Search menu reveals it (Gtk::SearchBar starts with search-mode
    // off by default, so nothing extra is needed here for that).
    m_vbox.pack_start(m_search_bar, PACK_SHRINK);

    // Paned layout for line numbers + text view
    m_vbox.pack_start(m_paned, PACK_EXPAND_WIDGET);
    m_paned.set_margin_top(0);
    m_paned.set_margin_bottom(0);
    m_paned.set_margin_start(0);
    m_paned.set_margin_end(0);
    
    m_paned.pack1(m_line_number_area, false, false);
    m_line_number_area.set_size_request(45, -1);
    m_line_number_area.set_margin_top(0);
    m_line_number_area.set_margin_bottom(0);
    m_line_number_area.set_margin_start(0);
    m_line_number_area.set_margin_end(0);

    m_paned.pack2(m_scrolled_window, true, false);
    m_scrolled_window.add(m_text_view);
    m_scrolled_window.set_policy(POLICY_AUTOMATIC, POLICY_AUTOMATIC);
    m_scrolled_window.set_margin_top(0);
    m_scrolled_window.set_margin_bottom(0);
    m_scrolled_window.set_margin_start(0);
    m_scrolled_window.set_margin_end(0);

    // Ensure the text view is editable
    m_text_view.set_editable(true);
    m_text_view.set_cursor_visible(true);

    show_all_children();
}

void ShellSheet::setup_menu() {
    auto* file_menu_item = Gtk::make_managed<Gtk::MenuItem>("_File", true);
    auto* file_menu = Gtk::make_managed<Gtk::Menu>();
    file_menu_item->set_submenu(*file_menu);

    m_item_new = Gtk::make_managed<Gtk::MenuItem>("_New", true);
    m_item_new->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_new));
    file_menu->append(*m_item_new);

    m_item_open = Gtk::make_managed<Gtk::MenuItem>("_Open", true);
    m_item_open->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_open));
    file_menu->append(*m_item_open);

    m_item_save = Gtk::make_managed<Gtk::MenuItem>("_Save", true);
    m_item_save->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_save));
    file_menu->append(*m_item_save);

    file_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_item_quit = Gtk::make_managed<Gtk::MenuItem>("_Quit", true);
    m_item_quit->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_quit));
    file_menu->append(*m_item_quit);

    m_menu_bar.append(*file_menu_item);

    auto* edit_menu_item = Gtk::make_managed<Gtk::MenuItem>("_Edit", true);
    auto* edit_menu = Gtk::make_managed<Gtk::Menu>();
    edit_menu_item->set_submenu(*edit_menu);

    m_item_undo = Gtk::make_managed<Gtk::MenuItem>("_Undo", true);
    m_item_undo->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_undo));
    m_item_undo->set_sensitive(false);
    edit_menu->append(*m_item_undo);

    m_item_redo = Gtk::make_managed<Gtk::MenuItem>("_Redo", true);
    m_item_redo->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_redo));
    m_item_redo->set_sensitive(false);
    edit_menu->append(*m_item_redo);

    edit_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_item_cut = Gtk::make_managed<Gtk::MenuItem>("Cu_t", true);
    m_item_cut->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_cut));
    edit_menu->append(*m_item_cut);

    m_item_copy = Gtk::make_managed<Gtk::MenuItem>("_Copy", true);
    m_item_copy->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_copy));
    edit_menu->append(*m_item_copy);

    m_item_paste = Gtk::make_managed<Gtk::MenuItem>("_Paste", true);
    m_item_paste->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_paste));
    edit_menu->append(*m_item_paste);

    edit_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_item_select_all = Gtk::make_managed<Gtk::MenuItem>("Select _All", true);
    m_item_select_all->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_select_all));
    edit_menu->append(*m_item_select_all);

    m_item_select_line = Gtk::make_managed<Gtk::MenuItem>("Select _Line", true);
    m_item_select_line->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_select_line));
    edit_menu->append(*m_item_select_line);

    m_menu_bar.append(*edit_menu_item);

    auto* view_menu_item = Gtk::make_managed<Gtk::MenuItem>("_View", true);
    auto* view_menu = Gtk::make_managed<Gtk::Menu>();
    view_menu_item->set_submenu(*view_menu);

    // Shortcuts are in the labels because zoom is handled in on_key_press()
    // rather than registered as accelerators - see setup_shortcuts().
    m_item_zoom_in = Gtk::make_managed<Gtk::MenuItem>("Zoom _In (Ctrl++)", true);
    m_item_zoom_in->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_zoom_in));
    view_menu->append(*m_item_zoom_in);

    m_item_zoom_out = Gtk::make_managed<Gtk::MenuItem>("Zoom _Out (Ctrl+-)", true);
    m_item_zoom_out->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_zoom_out));
    view_menu->append(*m_item_zoom_out);

    view_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_item_zoom_reset = Gtk::make_managed<Gtk::MenuItem>("_Reset Zoom (Ctrl+0)", true);
    m_item_zoom_reset->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_zoom_reset));
    view_menu->append(*m_item_zoom_reset);

    view_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    // A check item rather than a pair of Show/Hide entries: one menu
    // function that both reports the current state and toggles it.
    m_item_show_cwd = Gtk::make_managed<Gtk::CheckMenuItem>("Show Working _Directory", true);
    m_item_show_cwd->set_active(true);
    m_item_show_cwd->signal_toggled().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_toggle_cwd_bar));
    view_menu->append(*m_item_show_cwd);

    view_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    // Radio pair, not a check item: wrapping is a choice between two
    // states, and naming both makes it obvious that "no wrap" is a mode
    // rather than the absence of a feature. Purely visual - neither of
    // these ever alters the text (Text > Hard Wrap is the one that does).
    Gtk::RadioMenuItem::Group wrap_group;
    m_item_no_wrap = Gtk::make_managed<Gtk::RadioMenuItem>(wrap_group, "_No Wrap", true);
    m_item_soft_wrap = Gtk::make_managed<Gtk::RadioMenuItem>(wrap_group, "_Soft Wrap", true);
    m_item_no_wrap->set_active(true); // matches the TextView's existing default
    // Only one connection is needed: toggling either item emits on both.
    m_item_soft_wrap->signal_toggled().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_wrap_mode_changed));
    view_menu->append(*m_item_no_wrap);
    view_menu->append(*m_item_soft_wrap);

    m_menu_bar.append(*view_menu_item);

    auto* search_menu_item = Gtk::make_managed<Gtk::MenuItem>("_Search", true);
    auto* search_menu = Gtk::make_managed<Gtk::Menu>();
    search_menu_item->set_submenu(*search_menu);

    m_item_find = Gtk::make_managed<Gtk::MenuItem>("_Find", true);
    m_item_find->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_find));
    search_menu->append(*m_item_find);

    m_item_find_next = Gtk::make_managed<Gtk::MenuItem>("Find _Next", true);
    m_item_find_next->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_find_next));
    search_menu->append(*m_item_find_next);

    m_item_find_previous = Gtk::make_managed<Gtk::MenuItem>("Find Pre_vious", true);
    m_item_find_previous->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_find_previous));
    search_menu->append(*m_item_find_previous);

    search_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_item_replace = Gtk::make_managed<Gtk::MenuItem>("_Replace…", true);
    m_item_replace->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_replace));
    search_menu->append(*m_item_replace);

    m_menu_bar.append(*search_menu_item);

    // Te_xt, not _Text: _Terminal already owns the T mnemonic at the
    // menu-bar level.
    auto* text_menu_item = Gtk::make_managed<Gtk::MenuItem>("Te_xt", true);
    auto* text_menu = Gtk::make_managed<Gtk::Menu>();
    text_menu_item->set_submenu(*text_menu);

    auto add_text_item = [&](const char* label, void (ShellSheet::*handler)()) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(label, true);
        item->signal_activate().connect(sigc::mem_fun(*this, handler));
        text_menu->append(*item);
    };

    add_text_item("_UPPERCASE", &ShellSheet::on_transform_uppercase);
    add_text_item("_lowercase", &ShellSheet::on_transform_lowercase);
    add_text_item("_Title Case", &ShellSheet::on_transform_title_case);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("Sort _Ascending", &ShellSheet::on_transform_sort_ascending);
    add_text_item("Sort D_escending", &ShellSheet::on_transform_sort_descending);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    // Shortcuts live in the labels because Tab/Shift+Tab are handled in
    // on_key_press() rather than registered as accelerators - a Tab
    // accelerator would shadow the TextView's own focus/tab handling the
    // same way a Ctrl+C one shadowed Copy (see setup_shortcuts()).
    add_text_item("_Indent (Tab)", &ShellSheet::on_transform_indent);
    add_text_item("_Dedent (Shift+Tab)", &ShellSheet::on_transform_dedent);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("Dup_licate Line (Ctrl+Shift+D)", &ShellSheet::on_transform_duplicate_line);
    add_text_item("Move Line _Up (Alt+Up)", &ShellSheet::on_move_lines_up);
    add_text_item("Move Line Do_wn (Alt+Down)", &ShellSheet::on_move_lines_down);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("_Hard Wrap at 80 Columns", &ShellSheet::on_transform_hard_wrap);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("Tri_m Trailing Whitespace", &ShellSheet::on_transform_trim_trailing);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("Tabs to _Spaces", &ShellSheet::on_transform_tabs_to_spaces);
    add_text_item("S_paces to Tabs", &ShellSheet::on_transform_spaces_to_tabs);
    text_menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add_text_item("Line Endings: _Unix (LF)", &ShellSheet::on_transform_line_ending_lf);
    add_text_item("Line Endings: _Windows (CRLF)", &ShellSheet::on_transform_line_ending_crlf);
    add_text_item("Line Endings: _Classic Mac (CR)", &ShellSheet::on_transform_line_ending_cr);

    m_menu_bar.append(*text_menu_item);

    auto* terminal_menu_item = Gtk::make_managed<Gtk::MenuItem>("_Terminal", true);
    auto* terminal_menu = Gtk::make_managed<Gtk::Menu>();
    terminal_menu_item->set_submenu(*terminal_menu);

    m_item_terminal = Gtk::make_managed<Gtk::MenuItem>("_Run Command", true);
    m_item_terminal->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_terminal));
    terminal_menu->append(*m_item_terminal);

    // Shortcut lives in the label because it is handled in on_key_press()
    // rather than as an accelerator - see setup_shortcuts().
    m_item_terminal_window = Gtk::make_managed<Gtk::MenuItem>(
        "Run in Terminal _Window (Ctrl+Shift+Enter)", true);
    m_item_terminal_window->signal_activate().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_terminal_window));
    terminal_menu->append(*m_item_terminal_window);

    m_item_send_eof = Gtk::make_managed<Gtk::MenuItem>("Send _EOF", true);
    m_item_send_eof->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_send_eof));
    terminal_menu->append(*m_item_send_eof);

    // Shortcut is in the label because Ctrl+C is handled in on_key_press()
    // rather than registered as an accelerator - see setup_shortcuts().
    m_item_terminate = Gtk::make_managed<Gtk::MenuItem>("_Terminate (Ctrl+C)", true);
    m_item_terminate->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_terminate));
    terminal_menu->append(*m_item_terminate);

    m_menu_bar.append(*terminal_menu_item);

    auto* help_menu_item = Gtk::make_managed<Gtk::MenuItem>("_Help", true);
    auto* help_menu = Gtk::make_managed<Gtk::Menu>();
    help_menu_item->set_submenu(*help_menu);

    m_item_about = Gtk::make_managed<Gtk::MenuItem>("_About", true);
    m_item_about->signal_activate().connect(sigc::mem_fun(*this, &ShellSheet::on_menu_about));
    help_menu->append(*m_item_about);

    m_menu_bar.append(*help_menu_item);

    m_menu_bar.show_all();
}

void ShellSheet::setup_shortcuts() {
    auto accel_group = Gtk::AccelGroup::create();
    add_accel_group(accel_group);

    // File: Open
    m_item_open->add_accelerator("activate", accel_group, GDK_KEY_o, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
    
    // File: Save
    m_item_save->add_accelerator("activate", accel_group, GDK_KEY_s, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
    
    // File: Quit
    m_item_quit->add_accelerator("activate", accel_group, GDK_KEY_q, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
    
    // Terminal: Run Command (Ctrl+Enter)
    m_item_terminal->add_accelerator("activate", accel_group, GDK_KEY_Return, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
    m_item_terminal->add_accelerator("activate", accel_group, GDK_KEY_KP_Enter, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
    
    // Terminal: Send EOF (Ctrl+D)
    m_item_send_eof->add_accelerator("activate", accel_group, GDK_KEY_d, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Terminal: Terminate - deliberately NOT registered as an accelerator
    // here. GTK checks window accelerators before propagating a key press
    // to the focused widget, so a Ctrl+C accelerator silently shadows the
    // TextView's own Copy binding (verified: with it registered, Ctrl+A/X/V
    // all worked but Ctrl+C never copied). Ctrl+C is instead handled in
    // on_key_press(), which terminates only while a command is actually
    // running and otherwise lets Copy through - the terminal convention,
    // and no loss, since Terminate is a no-op when nothing is running.
    // The label carries the shortcut so it stays discoverable in the menu.
    
    // Edit: Undo (Ctrl+Z)
    m_item_undo->add_accelerator("activate", accel_group, GDK_KEY_z, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Edit: Redo (Ctrl+Shift+Z) - matches GNOME/gedit convention
    m_item_redo->add_accelerator("activate", accel_group, GDK_KEY_z, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Edit: Select Line (Ctrl+L)
    m_item_select_line->add_accelerator("activate", accel_group, GDK_KEY_l, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Edit: Cut/Copy/Paste/Select All deliberately get no accelerators here
    // - Ctrl+X/C/V/A already work directly in the TextView via GTK's own
    // built-in text-editing key bindings, regardless of any menu item. The
    // menu items exist for discoverability, not to add new shortcuts.
    // (Registering any of them here would in fact *break* the built-in
    // binding rather than duplicate it, for the accelerator-priority reason
    // described above for Terminate.)

    // Search: Find (Ctrl+F)
    m_item_find->add_accelerator("activate", accel_group, GDK_KEY_f, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Search: Find Next (Ctrl+G)
    m_item_find_next->add_accelerator("activate", accel_group, GDK_KEY_g, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Search: Find Previous (Ctrl+Shift+G)
    m_item_find_previous->add_accelerator("activate", accel_group, GDK_KEY_g, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));

    // Search: Replace (Ctrl+H)
    m_item_replace->add_accelerator("activate", accel_group, GDK_KEY_h, Gdk::CONTROL_MASK, static_cast<Gtk::AccelFlags>(GTK_ACCEL_VISIBLE));
}

void ShellSheet::on_scroll_changed() {
    m_line_number_area.queue_draw();
}

// While a command is running, Enter sends whatever text follows m_input_mark
// (i.e. whatever you've typed since output last arrived) to that command's
// stdin as a line - e.g. answering a "Do you want to continue? [Y/n]"
// prompt. m_input_mark has left gravity and is explicitly re-synced to
// m_command_mark's position on every output flush and every submitted line
// (see flush_output_buffer() and below), rather than left to drift on its
// own gravity - it must NOT advance when the user's own typed characters
// land at its position, only when we deliberately move it. Getting this
// backwards (e.g. reusing m_command_mark's right-gravity here) makes every
// captured line come out empty, since the mark would ride along with the
// very keystrokes it's supposed to bound.
//
// This still means freshly-arrived output can visually land ahead of text
// you're still typing if it arrives mid-line - a cosmetic quirk of not
// using a real terminal.
//
// Known limitation: this only fires on an actual Enter keypress, so a
// multi-line paste won't get forwarded line-by-line.
//
// When no command is running, Enter is left to the default handler (normal
// text editing).
bool ShellSheet::on_key_press(GdkEventKey* event) {
    // Ctrl+C: terminate the running command, the way it would at a
    // terminal - but only while one is actually running. With nothing
    // running there is nothing to terminate, so let the key fall through
    // to the TextView's own Copy binding instead of swallowing it. (This
    // lives here rather than as an accelerator precisely so Copy still
    // works; see setup_shortcuts().)
    if ((event->state & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)) {
        if (m_is_command_running) {
            on_menu_terminate();
            return true;
        }
        return false;
    }

    // Escape closes the search bar. GtkSearchBar already does this itself,
    // but only while the find entry has focus - click into the document
    // after a search and Escape would do nothing, leaving the bar stuck
    // open with no keyboard way out.
    if (event->keyval == GDK_KEY_Escape && m_search_bar.get_search_mode()) {
        m_search_bar.set_search_mode(false);
        return true;
    }

    // Ctrl+Shift+Enter forces a terminal launch. Checked before the plain
    // Enter handling below, which would otherwise treat it as input for a
    // running command.
    if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
        (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)) {
        on_menu_terminal_window();
        return true;
    }

    // Alt+Up / Alt+Down move the current line or selection. GtkTextView
    // binds Ctrl+Up/Down (paragraph movement) but leaves Alt+Up/Down free.
    if ((event->state & GDK_MOD1_MASK) && !(event->state & GDK_CONTROL_MASK)) {
        if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) {
            on_move_lines_up();
            return true;
        }
        if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) {
            on_move_lines_down();
            return true;
        }
    }

    // Ctrl+Shift+D duplicates. Plain Ctrl+D is already Send EOF, which a
    // worksheet needs more than it needs a second duplicate binding.
    if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
        (event->keyval == GDK_KEY_d || event->keyval == GDK_KEY_D)) {
        on_transform_duplicate_line();
        return true;
    }

    // Zoom. Handled here rather than as accelerators for the same reason
    // Ctrl+C is (see setup_shortcuts()). Ctrl+Shift++ arrives as plain
    // GDK_KEY_plus because '+' is already the shifted '=', so accepting
    // both plus and equal makes the gesture work with or without Shift.
    if (event->state & GDK_CONTROL_MASK) {
        switch (event->keyval) {
            case GDK_KEY_plus:
            case GDK_KEY_equal:
            case GDK_KEY_KP_Add:
                on_menu_zoom_in();
                return true;
            case GDK_KEY_minus:
            case GDK_KEY_underscore:
            case GDK_KEY_KP_Subtract:
                on_menu_zoom_out();
                return true;
            case GDK_KEY_0:
            case GDK_KEY_KP_0:
                on_menu_zoom_reset();
                return true;
            default:
                break;
        }
    }

    // Shift+Tab always dedents - it has no other useful meaning in a text
    // buffer. (X11 delivers Shift+Tab as ISO_Left_Tab, not Tab with a shift
    // modifier, so both keyvals are checked.)
    if (event->keyval == GDK_KEY_ISO_Left_Tab ||
        (event->keyval == GDK_KEY_Tab && (event->state & GDK_SHIFT_MASK))) {
        on_transform_dedent();
        return true;
    }
    // Plain Tab indents only when a selection spans more than one line.
    // With no selection, or one inside a single line, Tab has to keep
    // meaning "insert a tab character" - this is a shell worksheet, and
    // tab-completing or typing a literal tab is ordinary work here.
    if (event->keyval == GDK_KEY_Tab) {
        Gtk::TextIter sel_start, sel_end;
        if (m_text_buffer->get_selection_bounds(sel_start, sel_end) &&
            sel_start.get_line() != sel_end.get_line()) {
            on_transform_indent();
            return true;
        }
        return false;
    }

    if (!m_is_command_running || m_command_stdin_fd < 0) {
        return false;
    }
    if (event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter) {
        return false;
    }

    Gtk::TextIter mark_iter = m_input_mark->get_iter();
    Gtk::TextIter end_iter = m_text_buffer->end();
    std::string line = m_text_buffer->get_text(mark_iter, end_iter) + "\n";

    // Best-effort, deliberately: if the command isn't reading stdin (or
    // already closed it), this fails with EPIPE - SIGPIPE is ignored
    // process-wide (see main.cpp), so a failed write just does nothing
    // further. The fd is also non-blocking (see on_menu_terminal), so a
    // nearly-full 64KB pipe buffer could in principle take only part of the
    // line rather than blocking the UI waiting for the command to drain it.
    // Not worth looping over: it needs ~64KB of unread input to reach, and
    // the alternative (a blocking write on the main loop thread) trades a
    // rare truncated line for a frozen window.
    ::write(m_command_stdin_fd, line.c_str(), line.length());

    // Deliberately NOT setting m_force_new_undo_group here: this insert is
    // contiguous with the text the user just typed (normal typing, already
    // captured via on_buffer_insert), so letting it merge naturally is what
    // makes "submit a line" undo as one step - the typed response plus its
    // terminating newline together, not the newline alone.
    m_text_buffer->insert(m_text_buffer->end(), "\n");
    m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
    m_text_buffer->move_mark(m_input_mark, m_text_buffer->end());
    return true; // handled: suppress the default handler's own newline insert
}

bool ShellSheet::on_line_number_area_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    Gtk::Allocation allocation = m_line_number_area.get_allocation();
    double height = allocation.get_height();

    cr->set_source_rgb(0.15, 0.15, 0.15); // Darker sidebar
    cr->paint();

    cr->set_source_rgb(0.5, 0.5, 0.5);   // Gray numbers

    // The numbers are drawn with Pango in the TextView's OWN font, not with
    // Cairo's toy font API in a font of the gutter's own choosing. That is
    // what makes them track the editor: same family, same size, same
    // metrics, so they line up at any zoom level without a hand-tuned
    // baseline constant. (They used to need one, and it went stale the
    // moment anything about the font changed.)
    Pango::FontDescription font = text_view_font();
    int pixels_above = m_text_view.get_pixels_above_lines();

    // Get the vertical adjustment of the text view
    double scroll_offset = m_text_view.get_vadjustment()->get_value();

    // Get the visible range of the text view
    Gtk::TextIter start_iter, end_iter;
    int dummy_top;
    m_text_view.get_line_at_y(start_iter, scroll_offset, dummy_top);
    m_text_view.get_line_at_y(end_iter, scroll_offset + height, dummy_top);

    int start_line = start_iter.get_line();
    int end_line = end_iter.get_line();

    // Iterate only through visible lines
    for (int i = start_line; i <= end_line; ++i) {
        Gtk::TextIter iter = m_text_buffer->get_iter_at_line(i);
        
        int y_line, h_line;
        m_text_view.get_line_yrange(iter, y_line, h_line);

        // Calculate position relative to the top of the scrolled viewport
        double y = static_cast<double>(y_line) - scroll_offset;

        // Only draw if it's within the line number area's vertical bounds
        if (y + h_line >= 0 && y <= height) {
            // Pango draws from the layout's TOP-LEFT, not its baseline, so
            // this is the line's top plus whatever leading the TextView
            // puts above its own text - no baseline arithmetic needed.
            auto layout = m_line_number_area.create_pango_layout(std::to_string(i + 1));
            layout->set_font_description(font);
            cr->move_to(kGutterPadding, y + pixels_above);
            layout->show_in_cairo_context(cr);
        }
    }

    return true;
}

void ShellSheet::on_text_buffer_changed() {
    m_is_modified = true;
    // Without this the dirty flag was purely internal: the title's " *"
    // never appeared while editing, so there was no hint anywhere that the
    // document had unsaved changes. update_window_title() skips the actual
    // set_title() when nothing changed, so running this on every buffer
    // change (command output included) costs a string compare.
    update_window_title();
    // Widen the gutter if the document just grew an extra digit's worth of
    // lines. No-ops unless the digit count actually changed.
    update_line_number_width(/*force_remeasure=*/false);
    m_line_number_area.queue_draw();
    update_syntax_highlighting();
    if (m_search_bar.get_search_mode()) {
        // Re-tag matches for the edited text, but deliberately don't jump:
        // the *buffer* changed, not the search term, and moving the caret
        // here would make typing (or arriving command output) yank the
        // cursor to match #1 on every single change.
        refresh_search_matches(/*jump_to_first=*/false);
    }
}

namespace {

Glib::RefPtr<Gtk::TextTag> get_or_create_color_tag(
    const Glib::RefPtr<Gtk::TextBuffer>& buffer, const char* name, const char* color) {
    auto tag_table = buffer->get_tag_table();
    auto tag = tag_table->lookup(name);
    if (!tag) {
        tag = Gtk::TextTag::create(name);
        tag->set_property("foreground", Glib::ustring(color));
        tag_table->add(tag);
    }
    return tag;
}

// Same idea as get_or_create_color_tag(), but for a background highlight -
// used for search-match highlighting rather than syntax coloring.
Glib::RefPtr<Gtk::TextTag> get_or_create_background_tag(
    const Glib::RefPtr<Gtk::TextBuffer>& buffer, const char* name, const char* color) {
    auto tag_table = buffer->get_tag_table();
    auto tag = tag_table->lookup(name);
    if (!tag) {
        tag = Gtk::TextTag::create(name);
        tag->set_property("background", Glib::ustring(color));
        tag_table->add(tag);
    }
    return tag;
}

} // namespace

// Re-scans the whole buffer and reapplies shell syntax highlighting tags.
// Colors are chosen to read reasonably against a dark background.
//
// Note: compute_highlight_spans() reports columns as byte offsets into each
// line, while Gtk::TextIter's line offsets are counted in characters. These
// only diverge on lines containing multi-byte UTF-8 text before a span, so
// for the ASCII shell syntax this targets it's a non-issue in practice.
void ShellSheet::update_syntax_highlighting() {
    auto comment_tag = get_or_create_color_tag(m_text_buffer, "syntax_comment", "#6a9955");
    auto string_tag = get_or_create_color_tag(m_text_buffer, "syntax_string", "#ce9178");
    auto keyword_tag = get_or_create_color_tag(m_text_buffer, "syntax_keyword", "#569cd6");
    auto variable_tag = get_or_create_color_tag(m_text_buffer, "syntax_variable", "#4ec9b0");

    Gtk::TextIter buf_start = m_text_buffer->begin();
    Gtk::TextIter buf_end = m_text_buffer->end();
    m_text_buffer->remove_tag(comment_tag, buf_start, buf_end);
    m_text_buffer->remove_tag(string_tag, buf_start, buf_end);
    m_text_buffer->remove_tag(keyword_tag, buf_start, buf_end);
    m_text_buffer->remove_tag(variable_tag, buf_start, buf_end);

    for (const auto& span : compute_highlight_spans(m_text_buffer->get_text())) {
        Glib::RefPtr<Gtk::TextTag> tag;
        switch (span.kind) {
            case TokenKind::Comment:  tag = comment_tag;  break;
            case TokenKind::String:   tag = string_tag;   break;
            case TokenKind::Keyword:  tag = keyword_tag;  break;
            case TokenKind::Variable: tag = variable_tag; break;
        }
        Gtk::TextIter start = m_text_buffer->get_iter_at_line_offset(span.line, span.start_col);
        Gtk::TextIter end = m_text_buffer->get_iter_at_line_offset(span.line, span.end_col);
        m_text_buffer->apply_tag(tag, start, end);
    }
}

void ShellSheet::setup_search_bar() {
    auto* container = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    container->set_margin_top(4);
    container->set_margin_bottom(4);
    container->set_margin_start(4);
    container->set_margin_end(4);

    auto* find_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    m_find_entry.set_placeholder_text("Find");
    m_case_sensitive_check.set_tooltip_text("Case sensitive");
    m_regex_check.set_tooltip_text("Regular expression");
    find_row->pack_start(m_find_entry, true, true);
    find_row->pack_start(m_find_prev_button, false, false);
    find_row->pack_start(m_find_next_button, false, false);
    find_row->pack_start(m_case_sensitive_check, false, false);
    find_row->pack_start(m_regex_check, false, false);
    find_row->pack_start(m_match_count_label, false, false);

    auto* replace_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    m_replace_entry.set_placeholder_text("Replace");
    replace_row->pack_start(m_replace_entry, true, true);
    replace_row->pack_start(m_replace_button, false, false);
    replace_row->pack_start(m_replace_all_button, false, false);

    container->pack_start(*find_row, false, false);
    container->pack_start(*replace_row, false, false);

    m_search_bar.add(*container);
    // Lets the bar treat m_find_entry as its search entry even though it's
    // nested inside our own box rather than a direct child - this is what
    // wires up Escape-closes-bar for free.
    m_search_bar.connect_entry(m_find_entry);
    // ...but only while the entry has focus, and with nothing on screen to
    // say the bar can be dismissed at all. The built-in close button gives
    // it a visible, mouse-reachable way out; on_key_press() handles Escape
    // from the text view.
    m_search_bar.set_show_close_button(true);

    m_search_bar.property_search_mode_enabled().signal_changed().connect(
        sigc::mem_fun(*this, &ShellSheet::on_search_mode_changed));

    m_find_entry.signal_search_changed().connect(
        sigc::mem_fun(*this, &ShellSheet::update_search_matches));
    m_find_entry.signal_activate().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_find_next));
    m_find_next_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_find_next));
    m_find_prev_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ShellSheet::on_menu_find_previous));
    m_case_sensitive_check.signal_toggled().connect(
        sigc::mem_fun(*this, &ShellSheet::update_search_matches));
    m_regex_check.signal_toggled().connect(
        sigc::mem_fun(*this, &ShellSheet::update_search_matches));
    m_replace_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ShellSheet::on_replace_current));
    m_replace_all_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ShellSheet::on_replace_all));

    m_search_bar.show_all();
    update_search_matches(); // initializes button sensitivity/label for the empty-pattern state
}

// Re-scans the whole buffer for the current search term and re-tags every
// match. Called on every keystroke in the find entry (and whenever the
// case-sensitive/regex toggles change) while the search bar is visible -
// see on_text_buffer_changed() for the other trigger, a buffer edit.
//
// Deliberately has no special awareness of the buffer's live command-output
// region (see m_command_mark/m_input_mark elsewhere in this file): it will
// happily match inside already-printed command output, same as it would
// inside anything else in the buffer.
void ShellSheet::clear_search_highlight() {
    auto match_tag = get_or_create_background_tag(m_text_buffer, "search_match", "#3a3d41");
    auto current_tag = get_or_create_background_tag(m_text_buffer, "search_match_current", "#515c6a");

    Gtk::TextIter buf_start = m_text_buffer->begin();
    Gtk::TextIter buf_end = m_text_buffer->end();
    m_text_buffer->remove_tag(match_tag, buf_start, buf_end);
    m_text_buffer->remove_tag(current_tag, buf_start, buf_end);

    m_match_offsets.clear();
    m_current_match_index = -1;
    m_find_entry.get_style_context()->remove_class(GTK_STYLE_CLASS_ERROR);
}

void ShellSheet::on_search_mode_changed() {
    if (m_search_bar.get_search_mode()) {
        return; // opening is handled by whoever opened it
    }
    // Closing: the highlighting is part of the search UI, so it goes with
    // it - leaving matches tinted after the bar is gone looks like stray
    // selection the user can't get rid of. Focus goes back to the editor,
    // which is otherwise left on a widget that is no longer visible.
    clear_search_highlight();
    m_match_count_label.set_text("");
    m_text_view.grab_focus();
}

void ShellSheet::refresh_search_matches(bool jump_to_first) {
    auto match_tag = get_or_create_background_tag(m_text_buffer, "search_match", "#3a3d41");
    clear_search_highlight();

    std::string pattern = m_find_entry.get_text();
    bool has_matches = false;

    if (!pattern.empty()) {
        SearchOptions options;
        options.case_sensitive = m_case_sensitive_check.get_active();
        options.use_regex = m_regex_check.get_active();

        try {
            for (const auto& match : find_matches(m_text_buffer->get_text(), pattern, options)) {
                Gtk::TextIter start = m_text_buffer->get_iter_at_line_index(match.line, match.start_col);
                Gtk::TextIter end = m_text_buffer->get_iter_at_line_index(match.line, match.end_col);
                m_text_buffer->apply_tag(match_tag, start, end);
                m_match_offsets.push_back({start.get_offset(), end.get_offset()});
            }
            has_matches = !m_match_offsets.empty();
            m_match_count_label.set_text(std::to_string(m_match_offsets.size()) +
                                          (m_match_offsets.size() == 1 ? " match" : " matches"));
        } catch (const InvalidPatternError&) {
            m_find_entry.get_style_context()->add_class(GTK_STYLE_CLASS_ERROR);
            m_match_count_label.set_text("Invalid pattern");
        }
    } else {
        m_match_count_label.set_text("");
    }

    m_find_prev_button.set_sensitive(has_matches);
    m_find_next_button.set_sensitive(has_matches);
    m_replace_button.set_sensitive(has_matches);
    m_replace_all_button.set_sensitive(has_matches);

    if (has_matches && jump_to_first) {
        jump_to_match(0);
    }
}

// Selects match `index` (wrapping at either end of m_match_offsets),
// re-tagging it as the "current" match and scrolling it into view.
void ShellSheet::jump_to_match(int index) {
    if (m_match_offsets.empty()) return;
    int count = static_cast<int>(m_match_offsets.size());
    index = ((index % count) + count) % count;

    auto tag_table = m_text_buffer->get_tag_table();
    auto match_tag = tag_table->lookup("search_match");
    auto current_tag = tag_table->lookup("search_match_current");

    if (m_current_match_index >= 0 && m_current_match_index < count) {
        auto [prev_start_off, prev_end_off] = m_match_offsets[m_current_match_index];
        Gtk::TextIter prev_start = m_text_buffer->get_iter_at_offset(prev_start_off);
        Gtk::TextIter prev_end = m_text_buffer->get_iter_at_offset(prev_end_off);
        if (current_tag) m_text_buffer->remove_tag(current_tag, prev_start, prev_end);
        if (match_tag) m_text_buffer->apply_tag(match_tag, prev_start, prev_end);
    }

    m_current_match_index = index;
    auto [start_off, end_off] = m_match_offsets[index];
    Gtk::TextIter start = m_text_buffer->get_iter_at_offset(start_off);
    Gtk::TextIter end = m_text_buffer->get_iter_at_offset(end_off);
    if (match_tag) m_text_buffer->remove_tag(match_tag, start, end);
    if (current_tag) m_text_buffer->apply_tag(current_tag, start, end);

    m_text_buffer->select_range(start, end);
    m_text_view.scroll_to(start, 0.1);
}

void ShellSheet::on_menu_find() {
    m_search_bar.set_search_mode(true);
    m_find_entry.grab_focus();
}

void ShellSheet::on_menu_replace() {
    m_search_bar.set_search_mode(true);
    m_replace_entry.grab_focus();
}

void ShellSheet::on_menu_find_next() {
    if (!m_search_bar.get_search_mode()) {
        on_menu_find();
        return;
    }
    if (m_match_offsets.empty()) return;
    jump_to_match(m_current_match_index + 1);
}

void ShellSheet::on_menu_find_previous() {
    if (!m_search_bar.get_search_mode()) {
        on_menu_find();
        return;
    }
    if (m_match_offsets.empty()) return;
    jump_to_match(m_current_match_index - 1);
}

// Replaces just the current match. update_search_matches() re-runs
// automatically afterward via on_text_buffer_changed() (the erase/insert
// below fires the buffer's signal_changed()), so the match list, count, and
// current-match highlight are all recomputed from scratch rather than
// patched up here - simpler, at the cost of jumping back to the first
// match after every single replace rather than advancing to the next one.
void ShellSheet::on_replace_current() {
    if (m_match_offsets.empty() || m_current_match_index < 0) return;

    auto [start_off, end_off] = m_match_offsets[m_current_match_index];
    Gtk::TextIter start = m_text_buffer->get_iter_at_offset(start_off);
    Gtk::TextIter end = m_text_buffer->get_iter_at_offset(end_off);
    std::string replacement = m_replace_entry.get_text();

    // A replace is always its own undo step, never merged with typing on
    // either side of it.
    m_force_new_undo_group = true;
    m_text_buffer->erase(start, end);
    m_text_buffer->insert(m_text_buffer->get_iter_at_offset(start_off), replacement);
}

void ShellSheet::on_replace_all() {
    if (m_match_offsets.empty()) return;

    std::string replacement = m_replace_entry.get_text();
    // Replace from the last match back to the first: an edit at a given
    // position never shifts the offsets of matches earlier in the buffer,
    // so working backward means every stored offset is still valid when
    // its turn comes, without having to re-scan after each replacement.
    // Each substitution ends up as its own undo step (a Delete followed by
    // an Insert never merge with each other - different kinds - so setting
    // the flag once, before the loop, is enough to isolate the whole
    // operation from whatever preceded it).
    m_force_new_undo_group = true;
    for (auto it = m_match_offsets.rbegin(); it != m_match_offsets.rend(); ++it) {
        Gtk::TextIter start = m_text_buffer->get_iter_at_offset(it->first);
        Gtk::TextIter end = m_text_buffer->get_iter_at_offset(it->second);
        m_text_buffer->erase(start, end);
        m_text_buffer->insert(m_text_buffer->get_iter_at_offset(it->first), replacement);
    }
}

void ShellSheet::snap_range_to_lines(Gtk::TextIter& start, Gtk::TextIter& end) {
    start.set_line_offset(0);
    // Two cases already sit on a line boundary and must NOT be extended:
    // offset 0 (the range ends where a line begins - extending would pull in
    // a whole line the user never selected), and an end already at its
    // line's end. The latter matters because forward_to_line_end() on an
    // iterator that is *already* at a line end jumps to the end of the NEXT
    // line - so an unguarded call quietly swallows a following line.
    if (end.get_line_offset() != 0 && !end.ends_line()) {
        end.forward_to_line_end();
    }
}

void ShellSheet::apply_line_transform(
    const std::function<std::string(const std::string&)>& transform) {
    Gtk::TextIter start, end;
    if (!m_text_buffer->get_selection_bounds(start, end)) {
        // No selection: the caret's own line, not the whole buffer.
        start = end = m_text_buffer->get_iter_at_mark(m_text_buffer->get_insert());
        if (!end.ends_line()) end.forward_to_line_end();
    }
    snap_range_to_lines(start, end);
    apply_text_transform_to_range(start, end, transform);
}

void ShellSheet::apply_text_transform(
    const std::function<std::string(const std::string&)>& transform, bool snap_to_lines) {
    Gtk::TextIter start, end;
    bool has_selection = m_text_buffer->get_selection_bounds(start, end);

    if (!has_selection) {
        start = m_text_buffer->begin();
        end = m_text_buffer->end();
    } else if (snap_to_lines) {
        snap_range_to_lines(start, end);
    }

    apply_text_transform_to_range(start, end, transform);
}

void ShellSheet::apply_text_transform_to_range(
    Gtk::TextIter start, Gtk::TextIter end,
    const std::function<std::string(const std::string&)>& transform) {
    std::string original = m_text_buffer->get_text(start, end);
    std::string transformed = transform(original);
    if (transformed == original) return; // no-op: don't dirty modified-state for nothing

    int start_offset = start.get_offset();
    // A transform (selection-scoped or whole-buffer) is always its own
    // undo step, never merged with typing on either side of it.
    m_force_new_undo_group = true;
    size_t stack_before = m_undo_stack.size();
    m_text_buffer->erase(start, end);
    // Tie the insert below to that erase so the pair undoes as one action.
    // Guarded on the erase having actually recorded something: an erase of
    // an empty range records nothing, and linking to whatever unrelated
    // edit happened to be on top of the stack would group the transform
    // with a stranger.
    m_link_next_edit_to_previous = m_undo_stack.size() > stack_before;
    Gtk::TextIter insert_pos = m_text_buffer->get_iter_at_offset(start_offset);
    Gtk::TextIter new_end = m_text_buffer->insert(insert_pos, transformed);
    m_text_buffer->select_range(m_text_buffer->get_iter_at_offset(start_offset), new_end);
}

void ShellSheet::on_transform_uppercase() { apply_text_transform(&to_upper); }
void ShellSheet::on_transform_lowercase() { apply_text_transform(&to_lower); }
void ShellSheet::on_transform_title_case() { apply_text_transform(&to_title_case); }

void ShellSheet::on_transform_sort_ascending() {
    apply_text_transform([](const std::string& s) { return sort_lines(s, false, false); },
                          /*snap_to_lines=*/true);
}

void ShellSheet::on_transform_sort_descending() {
    apply_text_transform([](const std::string& s) { return sort_lines(s, false, true); },
                          /*snap_to_lines=*/true);
}

void ShellSheet::selected_line_range(int& first_line, int& last_line, bool& had_selection) {
    Gtk::TextIter start, end;
    had_selection = m_text_buffer->get_selection_bounds(start, end);
    if (!had_selection) {
        start = end = m_text_buffer->get_iter_at_mark(m_text_buffer->get_insert());
    }
    first_line = start.get_line();
    last_line = end.get_line();
    // A selection that stops exactly at the start of a line doesn't
    // include that line - the same rule snap_range_to_lines() applies.
    if (had_selection && last_line > first_line && end.get_line_offset() == 0) {
        --last_line;
    }
}

void ShellSheet::move_selected_lines(bool up) {
    int first_line = 0, last_line = 0;
    bool had_selection = false;
    selected_line_range(first_line, last_line, had_selection);

    // A buffer ending in a newline has one more "line" than it has content:
    // an empty final line that the caret can sit on. It must not take part
    // in a move. It is counted by get_line_count() but contributes no
    // element when the region is split into lines, so a region that
    // included it would come back one line short - and the rotation would
    // then quietly reorder the block itself instead of doing nothing.
    int bottom = m_text_buffer->get_line_count() - 1;
    Gtk::TextIter last_iter = m_text_buffer->get_iter_at_line(bottom);
    if (bottom > 0 && last_iter.starts_line() && last_iter.ends_line()) {
        --bottom;
    }
    if (first_line > bottom) return; // only the empty final line is involved
    last_line = std::min(last_line, bottom);

    // Nothing to trade places with at either end of the document.
    if (up && first_line == 0) return;
    if (!up && last_line >= bottom) return;

    int cursor_col = had_selection
        ? 0
        : m_text_buffer->get_iter_at_mark(m_text_buffer->get_insert()).get_line_offset();

    // The edited region is the moved block plus the single neighbouring
    // line it swaps with - not the whole document, so the undo step and
    // the buffer churn stay proportional to the move.
    int region_first = up ? first_line - 1 : first_line;
    int region_last = up ? last_line : last_line + 1;

    Gtk::TextIter region_start = m_text_buffer->get_iter_at_line(region_first);
    Gtk::TextIter region_end = m_text_buffer->get_iter_at_line(region_last);
    // Guarded for the same reason snap_range_to_lines() is: on an empty
    // line the iterator already ends the line, and forward_to_line_end()
    // would jump to the end of the following one.
    if (!region_end.ends_line()) region_end.forward_to_line_end();

    std::string region = m_text_buffer->get_text(region_start, region_end);
    std::string moved = up ? move_first_line_to_end(region) : move_last_line_to_start(region);
    if (moved == region) return;

    int region_offset = region_start.get_offset();
    // Erase + insert, grouped into a single undo step.
    m_force_new_undo_group = true;
    size_t stack_before = m_undo_stack.size();
    m_text_buffer->erase(region_start, region_end);
    m_link_next_edit_to_previous = m_undo_stack.size() > stack_before;
    m_text_buffer->insert(m_text_buffer->get_iter_at_offset(region_offset), moved);

    // Follow the text: keep the same lines selected (or the caret on the
    // same line and column), so the command can simply be repeated.
    int new_first = up ? first_line - 1 : first_line + 1;
    int new_last = up ? last_line - 1 : last_line + 1;

    if (had_selection) {
        Gtk::TextIter sel_start = m_text_buffer->get_iter_at_line(new_first);
        Gtk::TextIter sel_end = m_text_buffer->get_iter_at_line(new_last);
        if (!sel_end.ends_line()) sel_end.forward_to_line_end();
        m_text_buffer->select_range(sel_start, sel_end);
    } else {
        Gtk::TextIter caret = m_text_buffer->get_iter_at_line(new_first);
        Gtk::TextIter line_end = caret;
        if (!line_end.ends_line()) line_end.forward_to_line_end();
        caret.set_line_offset(std::min(cursor_col, line_end.get_line_offset()));
        m_text_buffer->place_cursor(caret);
    }
    m_text_view.scroll_to(m_text_buffer->get_insert());
}

void ShellSheet::on_move_lines_up() { move_selected_lines(true); }
void ShellSheet::on_move_lines_down() { move_selected_lines(false); }

void ShellSheet::on_transform_duplicate_line() {
    apply_line_transform(&duplicate_lines);
}

void ShellSheet::on_transform_hard_wrap() {
    apply_text_transform([](const std::string& s) { return hard_wrap(s, kHardWrapWidth); },
                          /*snap_to_lines=*/true);
}

void ShellSheet::on_transform_indent() {
    apply_line_transform([](const std::string& s) { return indent_lines(s, kIndentWidth); });
}

void ShellSheet::on_transform_dedent() {
    apply_line_transform([](const std::string& s) { return dedent_lines(s, kIndentWidth); });
}

void ShellSheet::on_transform_trim_trailing() {
    apply_text_transform(&trim_trailing_whitespace);
}

void ShellSheet::on_transform_tabs_to_spaces() {
    apply_text_transform([](const std::string& s) { return tabs_to_spaces(s); });
}

void ShellSheet::on_transform_spaces_to_tabs() {
    apply_text_transform([](const std::string& s) { return spaces_to_tabs(s); });
}

void ShellSheet::on_transform_line_ending_lf() {
    apply_text_transform([](const std::string& s) { return convert_line_endings(s, LineEnding::LF); });
}

void ShellSheet::on_transform_line_ending_crlf() {
    apply_text_transform([](const std::string& s) { return convert_line_endings(s, LineEnding::CRLF); });
}

void ShellSheet::on_transform_line_ending_cr() {
    apply_text_transform([](const std::string& s) { return convert_line_endings(s, LineEnding::CR); });
}

// --- Undo/Redo ---
// See docs/superpowers/specs/2026-09-16-undo-redo-design.md for the design
// this implements.

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

void ShellSheet::record_edit(EditRecord::Kind kind, int offset, const std::string& text) {
    auto now = std::chrono::steady_clock::now();
    long elapsed_ms = m_undo_stack.empty()
        ? 0
        : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_edit_time).count();
    m_last_edit_time = now;

    EditRecord new_record{kind, offset, text};
    // Only meaningful when there is something beneath it to continue.
    new_record.continues_previous = m_link_next_edit_to_previous && !m_undo_stack.empty();

    bool merged = false;
    if (!m_undo_stack.empty()) {
        merged = try_merge_edit(m_undo_stack.back(), new_record, elapsed_ms, m_force_new_undo_group);
    }
    if (!merged) {
        m_undo_stack.push_back(new_record);
    }
    m_force_new_undo_group = false;
    m_link_next_edit_to_previous = false;

    // A real (non-undo/redo) edit invalidates whatever could have been
    // redone.
    m_redo_stack.clear();
    update_undo_redo_sensitivity();
}

// Applies one record's inverse (for undo) or the record itself (for redo).
// `invert` is what distinguishes the two: an Insert record undoes by
// erasing and redoes by inserting.
void ShellSheet::apply_edit_record(const EditRecord& record, bool invert) {
    bool do_insert = (record.kind == EditRecord::Kind::Insert) ? !invert : invert;

    m_performing_undo_redo = true;
    if (do_insert) {
        Gtk::TextIter pos = m_text_buffer->get_iter_at_offset(record.offset);
        m_text_buffer->insert(pos, record.text);
        m_text_buffer->place_cursor(m_text_buffer->get_iter_at_offset(
            record.offset + static_cast<int>(record.text.length())));
    } else {
        Gtk::TextIter start = m_text_buffer->get_iter_at_offset(record.offset);
        Gtk::TextIter end = m_text_buffer->get_iter_at_offset(
            record.offset + static_cast<int>(record.text.length()));
        m_text_buffer->erase(start, end);
        m_text_buffer->place_cursor(m_text_buffer->get_iter_at_offset(record.offset));
    }
    m_performing_undo_redo = false;
}

void ShellSheet::undo() {
    if (m_undo_stack.empty()) return;

    // Drain a whole group, not just one record. A record marked
    // continues_previous is the tail of a multi-record action (currently:
    // the insert half of a text transform's erase+insert), so the record
    // beneath it has to come off too - stopping in between would leave the
    // document in a state it was never actually in.
    while (!m_undo_stack.empty()) {
        EditRecord record = m_undo_stack.back();
        m_undo_stack.pop_back();
        apply_edit_record(record, /*invert=*/true);
        m_redo_stack.push_back(record); // same record, opposite stack
        if (!record.continues_previous) break;
    }

    update_undo_redo_sensitivity();
}

void ShellSheet::redo() {
    if (m_redo_stack.empty()) return;

    // The mirror of undo's loop. The group's records sit on the redo stack
    // in reverse, so the *continuation* flag is on the record that comes
    // next rather than the one just applied - hence the peek-ahead.
    while (!m_redo_stack.empty()) {
        EditRecord record = m_redo_stack.back();
        m_redo_stack.pop_back();
        apply_edit_record(record, /*invert=*/false);
        m_undo_stack.push_back(record);
        if (m_redo_stack.empty() || !m_redo_stack.back().continues_previous) break;
    }

    update_undo_redo_sensitivity();
}

void ShellSheet::update_undo_redo_sensitivity() {
    if (m_item_undo) m_item_undo->set_sensitive(!m_undo_stack.empty());
    if (m_item_redo) m_item_redo->set_sensitive(!m_redo_stack.empty());
}

void ShellSheet::on_menu_undo() { undo(); }
void ShellSheet::on_menu_redo() { redo(); }

bool ShellSheet::on_delete_event(GdkEventAny* /*any_event*/) {
    // Returning true stops the close; false lets GTK's default handler hide
    // the window (which, being the last window, ends the application).
    return !prompt_save("closing");
}

void ShellSheet::on_menu_quit() {
    // close() rather than hide(): it sends the window a delete event, so
    // File > Quit and the window-manager close button go through the exact
    // same unsaved-changes check above instead of two paths that can drift.
    close();
}

void ShellSheet::on_menu_new() {
    if (prompt_save("starting a new document")) {
        m_force_new_undo_group = true;
        m_text_buffer->set_text("");
        m_current_file = "";
        m_is_modified = false;
        // Loading starts a fresh undo history - undoing back into a
        // different file's content would be confusing, not useful.
        m_undo_stack.clear();
        m_redo_stack.clear();
        update_undo_redo_sensitivity();
        update_window_title();
    }
}

void ShellSheet::on_menu_open() {
    if (prompt_save("opening another file")) {
        Gtk::FileChooserDialog dialog(*this, "Please choose a file", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("_Open", Gtk::RESPONSE_OK);

        int result = dialog.run();
        if (result == Gtk::RESPONSE_OK) {
            std::string filename = dialog.get_filename();
            std::ifstream file(filename);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                m_force_new_undo_group = true;
                m_text_buffer->set_text(buffer.str());
                m_current_file = filename;
                m_is_modified = false;
                // Loading starts a fresh undo history - undoing back into a
                // different file's content would be confusing, not useful.
                m_undo_stack.clear();
                m_redo_stack.clear();
                update_undo_redo_sensitivity();
                update_window_title();
            }
        }
    }
}

void ShellSheet::on_menu_save() {
    if (m_current_file.empty()) {
        Gtk::FileChooserDialog dialog(*this, "Please choose a file to save", Gtk::FILE_CHOOSER_ACTION_SAVE);
        dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("_Save", Gtk::RESPONSE_OK);
        dialog.set_do_overwrite_confirmation(true);

        int result = dialog.run();
        if (result == Gtk::RESPONSE_OK) {
            m_current_file = dialog.get_filename();
        } else {
            return;
        }
    }

    std::ofstream file(m_current_file);
    if (file.is_open()) {
        file << m_text_buffer->get_text();
        m_is_modified = false;
        update_window_title();
    }
}

std::pair<std::string, std::string> ShellSheet::get_version_info() {
    return {BUILD_NUMBER, ""};
}

void ShellSheet::on_menu_about() {
    std::string message = "By: Justin Sloan\nLicense: MIT\nBuild: " + BUILD_NUMBER;
    Gtk::MessageDialog dialog(*this, "About Shell Sheet", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
    dialog.set_secondary_text(message);
    dialog.run();
}

void ShellSheet::on_menu_terminal() { run_current_line(/*force_terminal=*/false); }

void ShellSheet::on_menu_terminal_window() { run_current_line(/*force_terminal=*/true); }

void ShellSheet::reap_launched_terminal(int, int) {
    // Intentionally empty. Connecting a child watch at all is what lets glib
    // reap the emulator process; there is nothing to report, because nothing
    // is running inside this app.
}

void ShellSheet::launch_in_terminal(const std::string& command,
                                     const Gtk::TextIter& line_end) {
    // Put the response where a command's output would have gone.
    m_text_buffer->move_mark(m_command_mark, line_end);
    m_text_buffer->insert(m_command_mark->get_iter(), "\n");
    m_text_buffer->move_mark(m_input_mark, m_command_mark->get_iter());

    const char* terminal_env = std::getenv("TERMINAL");
    TerminalEmulator terminal;
    auto on_path = [](const std::string& binary) {
        return !Glib::find_program_in_path(binary).empty();
    };

    if (!find_terminal_emulator(terminal_env ? terminal_env : "", on_path, terminal)) {
        // Deliberately does NOT fall back to running in the buffer: that is
        // what produces a screenful of escape sequences, which is the whole
        // problem this avoids.
        append_status_line("No terminal emulator found. Set $TERMINAL, or install "
                            "one (ptyxis, gnome-terminal, konsole, xterm, ...).");
        m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
        return;
    }

    // Note the absence of the `sudo -A` rewrite the in-buffer path does: in
    // a real terminal sudo prompts for itself, so the askpass helper is
    // neither needed nor wanted here.
    std::vector<std::string> argv_strings = build_terminal_argv(terminal, command);

    pid_t pid = fork();
    if (pid < 0) {
        append_status_line("Failed to launch terminal: fork failed");
        m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
        return;
    }

    if (pid == 0) { // Child
        // Its own session, so the terminal window outlives this app instead
        // of being torn down with it.
        ::setsid();
        // Start where the worksheet's tracked cd's have got to. Doing it
        // here rather than with each emulator's own --working-directory
        // flag keeps the argument table down to one shape.
        if (::chdir(m_working_dir.c_str()) != 0) {
            ::_exit(1);
        }

        std::vector<char*> c_argv;
        for (auto& arg : argv_strings) c_argv.push_back(&arg[0]);
        c_argv.push_back(nullptr);

        ::execvp(c_argv[0], c_argv.data());
        ::_exit(1); // _exit, not exit - see the comment in run_current_line()
    }

    // Reaped, but not tracked: nothing is running inside the app, so there
    // is no running indicator, no pipes and no stdin to forward.
    Glib::signal_child_watch().connect(
        sigc::mem_fun(*this, &ShellSheet::reap_launched_terminal), pid);

    append_status_line("Launched in " + terminal.binary + ": " + command);
    m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
}

void ShellSheet::run_current_line(bool force_terminal) {
    if (m_is_command_running) {
        on_menu_terminate();
        return;
    }

    Gtk::TextIter start, end;
    if (!m_text_view.get_buffer()->get_selection_bounds(start, end)) {
        start = m_text_buffer->get_iter_at_mark(m_text_view.get_buffer()->get_insert());
        end = start;
        end.forward_to_line_end();
    }
    std::string cmd_text = m_text_buffer->get_text(start, end);
    cmd_text.erase(0, cmd_text.find_first_not_of(" \t\n\r"));
    cmd_text.erase(cmd_text.find_last_not_of(" \t\n\r") + 1);

    if (cmd_text.empty()) return;

    // A plain `cd` is handled here rather than forked, because a `cd` in a
    // one-shot `bash -c` would change a directory that ceases to exist the
    // moment the command finishes. Compound lines like `cd foo && make`
    // are deliberately not caught by this (see directory_tracking.h) - they
    // still run through the shell and still work, they just don't move the
    // tracked directory.
    if (is_cd_command(cmd_text)) {
        run_cd_command(cmd_text, end);
        return;
    }

    // Full-screen programs (vim, htop, mc, watch...) cannot run in the
    // buffer at all, so they go to a real terminal instead. Everything
    // line-oriented - ping included, even though it runs indefinitely -
    // keeps running here exactly as before. See terminal_launch.h.
    if (force_terminal || needs_terminal(cmd_text)) {
        launch_in_terminal(cmd_text, end);
        return;
    }

    std::string shell_cmd = cmd_text;
    bool is_sudo = (shell_cmd.find("sudo ") == 0);

    // Let sudo itself prompt for the password (via SUDO_ASKPASS, wired up in
    // the forked child below) instead of us collecting it here. That way
    // sudo's own credential cache applies, and the password never passes
    // through this process at all.
    if (is_sudo) {
        shell_cmd = "sudo -A " + shell_cmd.substr(5);
    }

    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        std::cerr << "Failed to create pipes" << std::endl;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork" << std::endl;
        ::close(pipe_in[0]); ::close(pipe_in[1]);
        ::close(pipe_out[0]); ::close(pipe_out[1]);
        return;
    }

    if (pid == 0) { // Child
        // Run where the worksheet's tracked `cd`s have got to. Done in the
        // child so the app's own working directory is left alone (file
        // dialogs and relative paths in the parent keep behaving).
        if (::chdir(m_working_dir.c_str()) != 0) {
            // Plain write(), not perror/dprintf: after fork() in a threaded
            // process only async-signal-safe calls are safe, and stderr is
            // already pointed at the output pipe below anyway.
            const char msg[] = "shell_sheet: cannot enter working directory\n";
            ssize_t ignored = ::write(pipe_out[1], msg, sizeof(msg) - 1);
            (void)ignored;
            ::_exit(1);
        }

        ::dup2(pipe_in[0], STDIN_FILENO);
        ::dup2(pipe_out[1], STDOUT_FILENO);
        ::dup2(pipe_out[1], STDERR_FILENO); // merge stderr into the same stream as stdout

        for (int i = 0; i < 2; ++i) {
            ::close(pipe_in[i]);
            ::close(pipe_out[i]);
        }

        if (is_sudo) {
            // Point sudo at our own binary as its askpass helper. main()
            // recognizes SHELL_SHEET_ASKPASS and shows just the password
            // prompt instead of the normal editor window. Only affects this
            // child's environment, never the running app's.
            char self_path[4096];
            ssize_t len = ::readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
            if (len > 0) {
                self_path[len] = '\0';
                ::setenv("SUDO_ASKPASS", self_path, 1);
                ::setenv("SHELL_SHEET_ASKPASS", "1", 1);
            }
        }

        std::vector<std::string> argv_strs = {"/bin/bash", "-c", shell_cmd};
        std::vector<char*> c_argv;
        for (auto& s : argv_strs) c_argv.push_back(&s[0]);
        c_argv.push_back(nullptr);

        execvp(c_argv[0], c_argv.data());
        // _exit, not exit: this is a forked child of a GTK app, and exit()
        // would run atexit handlers and flush stdio buffers inherited from
        // the parent - re-emitting whatever the parent had buffered and
        // running global destructors in a process that only exists to be
        // replaced by execvp.
        ::_exit(1);
    } else { // Parent
        ::close(pipe_in[0]);
        ::close(pipe_out[1]);

        m_text_buffer->move_mark(m_command_mark, end);
        m_text_buffer->insert(m_command_mark->get_iter(), "\n");
        m_text_buffer->move_mark(m_input_mark, m_command_mark->get_iter());

        m_current_pid = pid;
        m_is_command_running = true;
        m_running_command = cmd_text;
        update_running_indicator();
        m_command_output_fd = pipe_out[0];
        // Kept open (instead of closed) so on_key_press() can forward typed
        // lines to the running command's stdin - e.g. answering apt's "Do
        // you want to continue? [Y/n]" prompt.
        m_command_stdin_fd = pipe_in[1];

        // Both ends non-blocking, because every read/write on them happens
        // on the GTK main loop thread - anything that blocks there freezes
        // the whole UI.
        //
        // The read end matters most: a command that backgrounds something
        // ("./server &") leaves a grandchild holding the write end open
        // long after the command itself exits, so handle_process_exit()'s
        // drain loop would otherwise block forever waiting on a writer that
        // may never close. The write end is the same story with a full
        // 64KB pipe buffer and a command that never reads its stdin.
        ::fcntl(m_command_output_fd, F_SETFL, ::fcntl(m_command_output_fd, F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(m_command_stdin_fd, F_SETFL, ::fcntl(m_command_stdin_fd, F_GETFL, 0) | O_NONBLOCK);
        update_window_title();

        m_io_conn = Glib::signal_io().connect(
            sigc::mem_fun(*this, &ShellSheet::on_command_output_received),
            m_command_output_fd,
            Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR
        );

        // Reap the child through glib's SIGCHLD machinery so it never lingers as a zombie.
        m_child_watch_conn = Glib::signal_child_watch().connect(
            sigc::mem_fun(*this, &ShellSheet::handle_process_exit),
            pid
        );
    }
}

void ShellSheet::on_menu_terminate() {
    if (m_is_command_running && m_current_pid > 0) {
        kill(m_current_pid, SIGTERM);
    }
}

// Closes the write end of the running command's stdin pipe, delivering EOF
// to it - the equivalent of Ctrl+D at a real terminal. Needed for commands
// that read until stdin closes rather than waiting for a line (cat without
// arguments, python3's REPL, sort, ...); on_key_press() alone can only ever
// send lines, never signal "no more input is coming."
void ShellSheet::on_menu_send_eof() {
    if (m_command_stdin_fd >= 0) {
        ::close(m_command_stdin_fd);
        m_command_stdin_fd = -1;
    }
}

// Cut/copy/paste triggered via Ctrl+X/C/V go straight through GTK's own
// built-in TextView key bindings and never reach these functions at all -
// only a menu click does (see setup_shortcuts()'s comment on why those
// keys get no accelerator of ours). So m_force_new_undo_group here only
// guarantees a menu-triggered Cut/Paste is always its own undo step; a
// keyboard-triggered one is subject to the normal contiguity/timing merge
// like any other edit. Narrow, accepted gap: making the keyboard path
// equally deliberate would mean intercepting Ctrl+X/V ourselves instead of
// leaving them to GTK's default handling, which isn't worth the added
// complexity for how rarely it'd actually change anything (it would take a
// cut/paste landing contiguously against another edit within ~700ms).

void ShellSheet::on_menu_cut() {
    // Only arm the flag if the cut will actually change something. Setting
    // it for a no-op cut (nothing selected) would leave it armed, wrongly
    // breaking undo grouping for whatever unrelated edit came next.
    Gtk::TextIter start, end;
    if (m_text_buffer->get_selection_bounds(start, end)) {
        m_force_new_undo_group = true;
    }
    m_text_buffer->cut_clipboard(Gtk::Clipboard::get());
}

void ShellSheet::on_menu_copy() {
    m_text_buffer->copy_clipboard(Gtk::Clipboard::get());
}

void ShellSheet::on_menu_paste() {
    m_force_new_undo_group = true;
    m_text_buffer->paste_clipboard(Gtk::Clipboard::get());
}

void ShellSheet::on_menu_select_all() {
    m_text_buffer->select_range(m_text_buffer->begin(), m_text_buffer->end());
}

// Selects the whole line containing the cursor (or the start of the
// current selection, if there is one). Used to be "Highlight Line", which
// applied a tag whose background color was never actually set - so it had
// no visible effect at all beyond moving the cursor. A real selection is
// both more useful and more honest about what it does.
void ShellSheet::on_menu_select_line() {
    Gtk::TextIter start, end;
    if (!m_text_buffer->get_selection_bounds(start, end)) {
        start = m_text_buffer->get_insert()->get_iter();
    }

    start.set_line_offset(0);
    end = start;
    end.forward_to_line_end();
    m_text_buffer->select_range(start, end);
}

void ShellSheet::on_menu_bashrc() {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    if (!home.empty()) {
        std::string path = home + "/.bashrc";
        std::ifstream file(path);
        m_force_new_undo_group = true;
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            m_text_buffer->set_text(buffer.str());
            m_current_file = path;
            m_is_modified = false;
            update_window_title();
        } else {
            m_current_file = path;
            m_text_buffer->set_text("");
            m_is_modified = false;
            update_window_title();
        }
        // Loading starts a fresh undo history, same as Open/New.
        m_undo_stack.clear();
        m_redo_stack.clear();
        update_undo_redo_sensitivity();
    }
}

void ShellSheet::on_menu_hosts() {
    std::string path = "/etc/hosts";
    std::ifstream file(path);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        m_force_new_undo_group = true;
        m_text_buffer->set_text(buffer.str());
        m_current_file = path;
        m_is_modified = false;
        // Loading starts a fresh undo history, same as Open/New.
        m_undo_stack.clear();
        m_redo_stack.clear();
        update_undo_redo_sensitivity();
        update_window_title();
    }
}

bool ShellSheet::prompt_save(const char* action_description) {
    if (!m_is_modified) {
        return true;
    }

    std::string document = m_current_file.empty()
        ? "this document"
        : Glib::path_get_basename(m_current_file);

    Gtk::MessageDialog dialog(*this,
                               "Save changes to " + document + " before " +
                                   action_description + "?",
                               false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE);
    dialog.set_secondary_text("If you don't save, your changes will be permanently lost.");

    // Three buttons, not Yes/No. With only Yes/No there is no visible way to
    // back out of a quit at all - the "cancel" path exists but is reachable
    // only by guessing that Escape works.
    dialog.add_button("Close _without Saving", Gtk::RESPONSE_NO);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Save", Gtk::RESPONSE_YES);
    dialog.set_default_response(Gtk::RESPONSE_YES);

    int result = dialog.run();

    if (result == Gtk::RESPONSE_YES) {
        on_menu_save();
        // Saving can still be abandoned (Cancel in the file chooser, or an
        // unwritable path), and in that case the changes are still unsaved -
        // so the caller must not go ahead and discard them.
        return !m_is_modified;
    }
    if (result == Gtk::RESPONSE_NO) {
        return true; // discard deliberately
    }
    // Cancel, Escape, or the dialog's own close button: abort the action.
    return false;
}

void ShellSheet::update_window_title() {
    auto [build_num, version_str] = get_version_info();
    std::string title = "Shell Sheet";
    if (m_is_modified) title += " *";
    if (m_is_command_running) title += " — Running";
    // Called on every buffer change, so don't churn the window manager with
    // a set_title() that changes nothing.
    if (get_title() != title) set_title(title);
}

bool ShellSheet::flush_output_buffer() {
    if (m_output_buffer.empty()) {
        return false;
    }
    
    std::string block;
    while (!m_output_buffer.empty()) {
        block += m_output_buffer.front();
        m_output_buffer.pop_front();
    }
    
    Gtk::TextIter insert_pos = m_command_mark->get_iter();
    // A flush is always its own undo step, never merged with typing on
    // either side of it.
    m_force_new_undo_group = true;
    m_text_buffer->insert(insert_pos, block);
    // Pending typed-but-not-yet-submitted input, if any, starts fresh after
    // this newly-arrived output (see on_key_press).
    m_text_buffer->move_mark(m_input_mark, m_command_mark->get_iter());

    m_line_number_area.queue_draw();
    return true;
}

bool ShellSheet::on_command_output_received(Glib::IOCondition condition) {
    if (m_command_output_fd < 0) return false;

    if (condition & Glib::IO_IN) {
        char buffer[4096];
        ssize_t bytes_read = ::read(m_command_output_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            m_output_buffer.push_back(std::string(buffer));

            if (!m_output_timeout_conn.connected()) {
                m_output_timeout_conn = Glib::signal_timeout().connect(
                    sigc::mem_fun(*this, &ShellSheet::flush_output_buffer),
                    100
                );
            }
            return true;
        }
        // The fd is non-blocking (see on_menu_terminal), so "no data right
        // now" is EAGAIN, not EOF - keep watching rather than tearing the
        // watch down on what is only a spurious wakeup.
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
    }

    // EOF or an error on the pipe: nothing more to read. Flush whatever is left
    // right away instead of waiting on the timeout, and stop watching this fd.
    // Actual process bookkeeping (marking the command as finished, reaping the
    // child) happens in handle_process_exit(), driven by the child watch below,
    // so the exit status is never guessed at here.
    flush_output_buffer();
    return false;
}

void ShellSheet::append_status_line(const std::string& text) {
    Gtk::TextIter pos = m_command_mark->get_iter();

    std::string line;
    // Only break the line first if the command's output didn't already end
    // on one - otherwise a command whose last write had no trailing newline
    // would get the status glued onto the end of its final line.
    if (!pos.starts_line()) {
        line += "\n";
    }
    line += text + "\n";

    // Same conventions as flush_output_buffer(): its own undo step, and
    // pending typed input starts after it.
    m_force_new_undo_group = true;
    m_text_buffer->insert(pos, line);
    m_text_buffer->move_mark(m_input_mark, m_command_mark->get_iter());
    m_line_number_area.queue_draw();
}

void ShellSheet::handle_process_exit(int pid, int status) {
    if (m_is_command_running && pid == m_current_pid) {
        m_is_command_running = false;
        m_current_pid = -1;
        update_running_indicator();

        // The child watch and the output-ready watch race each other: glib
        // gives no guarantee which fires first once the process exits. Stop
        // the io watch before touching the fd, then drain whatever output is
        // still sitting unread in the pipe so nothing gets lost either way.
        if (m_io_conn.connected()) {
            m_io_conn.disconnect();
        }
        if (m_child_watch_conn.connected()) {
            m_child_watch_conn.disconnect();
        }

        if (m_command_output_fd >= 0) {
            // Drain whatever is still buffered in the pipe. The fd is
            // non-blocking, so this stops at EAGAIN rather than hanging when
            // a backgrounded grandchild still holds the write end open - a
            // blocking read here would freeze the entire UI until that
            // grandchild exited, which for something like "./server &" could
            // be never. Anything that grandchild writes afterward is
            // deliberately discarded: the command we were running is done.
            char buffer[4096];
            ssize_t bytes_read;
            while ((bytes_read = ::read(m_command_output_fd, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes_read] = '\0';
                m_output_buffer.push_back(std::string(buffer));
            }
            ::close(m_command_output_fd);
            m_command_output_fd = -1;
        }

        if (m_command_stdin_fd >= 0) {
            ::close(m_command_stdin_fd);
            m_command_stdin_fd = -1;
        }

        flush_output_buffer();

        // Report an abnormal end the way a shell does. Ctrl+C sends SIGTERM
        // (see on_menu_terminate), so that case prints "Terminated";
        // strsignal() gives the right wording for every other signal too
        // ("Killed", "Segmentation fault", ...) rather than silently
        // labelling every signal death as a termination.
        if (WIFSIGNALED(status)) {
            append_status_line(strsignal(WTERMSIG(status)));
        }

        m_text_buffer->move_mark(m_command_mark, m_text_buffer->end());
        update_window_title();
    }
}