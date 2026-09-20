#ifndef SHELL_SHEET_H
#define SHELL_SHEET_H

#include <gtkmm.h>
#include <glibmm.h>
#include <chrono>
#include <string>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include "directory_tracking.h"
#include "text_search.h"
#include "text_transforms.h"
#include "undo_stack.h"

class ShellSheet : public Gtk::Window {
public:
    ShellSheet();
    virtual ~ShellSheet();

    // Sends SIGTERM to the currently-running command, if any. Public so
    // main.cpp's SIGTERM/SIGINT handler can ask the app to clean up its
    // child before quitting - see the comment there for what this can and
    // can't catch (notably, never SIGKILL).
    void terminate_running_command() { on_menu_terminate(); }

protected:
    // UI Components
    Gtk::Box m_vbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::MenuBar m_menu_bar;
    Gtk::ScrolledWindow m_scrolled_window;
    Gtk::TextView m_text_view;
    Gtk::DrawingArea m_line_number_area;
    Gtk::Paned m_paned{Gtk::ORIENTATION_HORIZONTAL};

    // Editor font. The size is in points (CSS pt), applied to the TextView
    // through m_font_provider; the line-number gutter reads the TextView's
    // resolved font back out rather than keeping its own copy, so the two
    // can never disagree about size or metrics.
    static constexpr int kDefaultFontSize = 11;
    static constexpr int kMinFontSize = 6;
    static constexpr int kMaxFontSize = 48;
    int m_font_size = kDefaultFontSize;
    Glib::RefPtr<Gtk::CssProvider> m_font_provider;
    // Digit count the gutter is currently sized for, so an ordinary edit
    // doesn't re-measure and re-position the pane on every keystroke.
    int m_line_number_digits = 0;

    // Working directory every command is run in. Each command is its own
    // `bash -c`, so a `cd` inside one would die with it - instead `cd`
    // lines are resolved here and applied to later commands. See
    // directory_tracking.h and run_cd_command().
    Gtk::Revealer m_cwd_revealer;
    Gtk::Label m_cwd_label;
    std::string m_working_dir;
    // Where `cd -` goes back to. Empty until the first successful cd.
    std::string m_previous_working_dir;

    // Shown only while a command is actually running. The buffer scrolls,
    // and the title bar's " - Running" suffix is easy to miss, so this is
    // the one unmissable signal that the app is busy.
    Gtk::Revealer m_running_revealer;
    Gtk::Label m_running_label;
    Glib::RefPtr<Gtk::CssProvider> m_running_css_provider;
    // The command the indicator is currently naming.
    std::string m_running_command;

    // State
    std::string m_current_file;
    bool m_is_modified = false;
    bool m_is_command_running = false;
    int m_current_pid = -1;
    std::deque<std::string> m_output_buffer;
    sigc::connection m_output_timeout_conn;
    sigc::connection m_io_conn;
    sigc::connection m_child_watch_conn;
    Glib::RefPtr<Gtk::TextBuffer> m_text_buffer;
    Glib::RefPtr<Gtk::TextMark> m_command_mark;
    Glib::RefPtr<Gtk::TextMark> m_input_mark;
    int m_command_output_fd = -1;
    int m_command_stdin_fd = -1;

    // Find & Replace
    Gtk::SearchBar m_search_bar;
    Gtk::SearchEntry m_find_entry;
    Gtk::Entry m_replace_entry;
    Gtk::Button m_find_prev_button{"Previous"};
    Gtk::Button m_find_next_button{"Next"};
    Gtk::Button m_replace_button{"Replace"};
    Gtk::Button m_replace_all_button{"Replace All"};
    Gtk::CheckButton m_case_sensitive_check{"Aa"};
    Gtk::CheckButton m_regex_check{".*"};
    Gtk::Label m_match_count_label;
    // Buffer char offsets [start, end) of every current match, in document
    // order. Recomputed on every keystroke while the bar is visible - see
    // update_search_matches().
    std::vector<std::pair<int, int>> m_match_offsets;
    int m_current_match_index = -1;

    // Undo/Redo
    std::vector<EditRecord> m_undo_stack;
    std::vector<EditRecord> m_redo_stack;
    // Set around undo()/redo()'s own buffer mutations so on_buffer_insert/
    // on_buffer_erase don't record them as new edits (that would corrupt
    // both stacks - see docs/superpowers/specs/2026-09-16-undo-redo-design.md).
    bool m_performing_undo_redo = false;
    // Set by any of our own direct buffer-mutating call sites (paste,
    // command output, Replace/Replace All, text transforms, stdin-echo,
    // file load) immediately before mutating, so that edit is always its
    // own standalone undo step - never silently merged with adjacent
    // typing in either direction. Consumed (and cleared) by the next
    // recorded edit.
    bool m_force_new_undo_group = false;
    // The mirror image of m_force_new_undo_group: set immediately before the
    // *second* half of a replace (the insert that follows an erase) so the
    // two records undo and redo as a single step. Consumed (and cleared) by
    // the next recorded edit, same as the flag above.
    bool m_link_next_edit_to_previous = false;
    std::chrono::steady_clock::time_point m_last_edit_time;

    // Menu actions
    void on_menu_undo();
    void on_menu_redo();
    void on_menu_new();
    void on_menu_open();
    void on_menu_save();
    void on_menu_quit();
    void on_menu_terminal();
    void on_menu_terminate();
    void on_menu_send_eof();
    void on_menu_bashrc();
    void on_menu_hosts();
    void on_menu_about();
    void on_menu_cut();
    void on_menu_copy();
    void on_menu_paste();
    void on_menu_select_all();
    void on_menu_select_line();
    void on_menu_zoom_in();
    void on_menu_zoom_out();
    void on_menu_zoom_reset();
    void on_menu_toggle_cwd_bar();
    void on_menu_wrap_mode_changed();
    void on_menu_find();
    void on_menu_find_next();
    void on_menu_find_previous();
    void on_menu_replace();
    void on_transform_uppercase();
    void on_transform_lowercase();
    void on_transform_title_case();
    void on_transform_sort_ascending();
    void on_transform_sort_descending();
    void on_transform_indent();
    void on_transform_dedent();
    void on_transform_duplicate_line();
    void on_transform_hard_wrap();
    void on_move_lines_up();
    void on_move_lines_down();
    void on_transform_trim_trailing();
    void on_transform_tabs_to_spaces();
    void on_transform_spaces_to_tabs();
    void on_transform_line_ending_lf();
    void on_transform_line_ending_crlf();
    void on_transform_line_ending_cr();

    // Helpers
    // Offers to save when the buffer is dirty, and reports whether the
    // caller may proceed with whatever it was about to do. Returns false
    // only when the user actively backed out (Cancel/Escape) or when Save
    // was chosen but did not succeed - in both cases the pending action
    // must be abandoned, not carried out. `action_description` completes
    // the sentence "Save changes to <file> before ...?".
    bool prompt_save(const char* action_description = "continuing");

    // Vetoes a window close (X button, or File > Quit, which routes here
    // via close()) while there are unsaved changes the user wants to keep.
    bool on_delete_event(GdkEventAny* any_event) override;
    std::pair<std::string, std::string> get_version_info();
    void update_window_title();
    bool flush_output_buffer();
    bool on_command_output_received(Glib::IOCondition condition);
    void handle_process_exit(int pid, int status);
    // Appends a one-line status notice (e.g. "Terminated") after a
    // command's output, starting a new line first if the output didn't.
    void append_status_line(const std::string& text);
    bool on_line_number_area_draw(const Cairo::RefPtr<Cairo::Context>& cr);
    // The font the TextView is actually rendering with, resolved from its
    // style context. The gutter draws its numbers with this same
    // description, which is what keeps the two aligned at every size.
    Pango::FontDescription text_view_font() const;
    // Clamps to [kMinFontSize, kMaxFontSize] and re-applies if it changed.
    void set_font_size(int points);
    void apply_font_size();
    void redraw_line_numbers();
    // Sizes the gutter to exactly fit the largest line number, growing
    // AND shrinking with the digit count (1-9 lines is one character wide,
    // 10-99 two, and so on). force_remeasure re-measures even when the
    // digit count is unchanged, which is what a font-size change needs.
    void update_line_number_width(bool force_remeasure);
    void on_text_buffer_changed();
    void on_scroll_changed();
    void update_syntax_highlighting();
    bool on_key_press(GdkEventKey* event);
    void setup_search_bar();
    void setup_cwd_bar();
    void setup_running_bar();
    // Reveals or hides the running indicator to match m_is_command_running.
    void update_running_indicator();
    // Repaints the working-directory bar from m_working_dir.
    void update_working_directory_display();
    // Applies a `cd` line itself instead of forking a shell for it, and
    // reports the outcome into the buffer the way a command's output
    // would appear. `line_end` is the end of the command line, so the
    // response lands directly under it.
    void run_cd_command(const std::string& command, const Gtk::TextIter& line_end);
    // Rebuilds the match list from the current search term. The no-argument
    // form (used as the signal handler for the find entry and the
    // case/regex toggles) also jumps to the first match, since that's what
    // you want while typing a search. refresh_search_matches(false) is for
    // the case where the *buffer* changed rather than the search term - it
    // must NOT move the cursor, or editing the document with the bar open
    // would yank the caret to match #1 after every keystroke.
    void update_search_matches() { refresh_search_matches(true); }
    void refresh_search_matches(bool jump_to_first);
    // Drops the match tags and the match bookkeeping. Shared by
    // refresh_search_matches (which re-tags immediately afterwards) and by
    // closing the bar (which doesn't).
    void clear_search_highlight();
    // Runs whenever the search bar is revealed or dismissed; cleans up
    // after a dismissal.
    void on_search_mode_changed();
    void jump_to_match(int index);
    void on_replace_current();
    void on_replace_all();
    // Shared BBEdit-style scoping: operates on the current selection if one
    // exists, otherwise the whole buffer. snap_to_lines widens a partial
    // selection out to full line boundaries first - needed by Sort Lines
    // (a partial-line selection would otherwise corrupt neighboring lines'
    // content); the other transforms don't need it.
    void apply_text_transform(const std::function<std::string(const std::string&)>& transform,
                               bool snap_to_lines = false);
    // Indent/Dedent scoping, deliberately different from apply_text_transform's:
    // the selection if there is one, otherwise just the CURRENT LINE - never
    // the whole buffer. Pressing Tab must not silently re-indent the entire
    // document. Always snapped to line boundaries.
    void apply_line_transform(const std::function<std::string(const std::string&)>& transform);
    // Moves the selected lines (or the caret's line) one line up or down,
    // trading places with the neighbour. Not an apply_line_transform: it
    // has to reach outside the selection to pick up that neighbour, and it
    // keeps the moved lines selected so the command can be repeated.
    void move_selected_lines(bool up);
    // The lines a command should act on: the selection widened to whole
    // lines, or the caret's line when there is no selection. Reports the
    // range as line numbers, which is what the line-oriented commands
    // actually want.
    void selected_line_range(int& first_line, int& last_line, bool& had_selection);
    // Widens [start, end) out to whole lines. Shared by both scoping helpers.
    void snap_range_to_lines(Gtk::TextIter& start, Gtk::TextIter& end);
    // The actual edit, once a scoping helper has decided the range: run the
    // transform, and if it changed anything, replace the range with the
    // result as a single undo step and leave it selected.
    void apply_text_transform_to_range(Gtk::TextIter start, Gtk::TextIter end,
                                        const std::function<std::string(const std::string&)>& transform);

    // Undo/Redo - see docs/superpowers/specs/2026-09-16-undo-redo-design.md
    void on_buffer_insert(const Gtk::TextBuffer::iterator& pos, const Glib::ustring& text, int bytes);
    void on_buffer_erase(const Gtk::TextBuffer::iterator& start, const Gtk::TextBuffer::iterator& end);
    void record_edit(EditRecord::Kind kind, int offset, const std::string& text);
    void apply_edit_record(const EditRecord& record, bool invert);
    void undo();
    void redo();
    void update_undo_redo_sensitivity();

private:
    // Menu items kept as members to allow for accelerators
    Gtk::MenuItem* m_item_undo = nullptr;
    Gtk::MenuItem* m_item_redo = nullptr;
    Gtk::MenuItem* m_item_new = nullptr;
    Gtk::MenuItem* m_item_open = nullptr;
    Gtk::MenuItem* m_item_save = nullptr;
    Gtk::MenuItem* m_item_quit = nullptr;
    Gtk::MenuItem* m_item_terminal = nullptr;
    Gtk::MenuItem* m_item_send_eof = nullptr;
    Gtk::MenuItem* m_item_terminate = nullptr;
    Gtk::MenuItem* m_item_cut = nullptr;
    Gtk::MenuItem* m_item_copy = nullptr;
    Gtk::MenuItem* m_item_paste = nullptr;
    Gtk::MenuItem* m_item_select_all = nullptr;
    Gtk::MenuItem* m_item_select_line = nullptr;
    Gtk::MenuItem* m_item_zoom_in = nullptr;
    Gtk::MenuItem* m_item_zoom_out = nullptr;
    Gtk::MenuItem* m_item_zoom_reset = nullptr;
    Gtk::CheckMenuItem* m_item_show_cwd = nullptr;
    Gtk::RadioMenuItem* m_item_no_wrap = nullptr;
    Gtk::RadioMenuItem* m_item_soft_wrap = nullptr;
    Gtk::MenuItem* m_item_about = nullptr;
    Gtk::MenuItem* m_item_find = nullptr;
    Gtk::MenuItem* m_item_find_next = nullptr;
    Gtk::MenuItem* m_item_find_previous = nullptr;
    Gtk::MenuItem* m_item_replace = nullptr;

    void setup_ui_layout();
    void apply_css();
    void setup_menu();
    void setup_shortcuts();
};

#endif // SHELL_SHEET_H
