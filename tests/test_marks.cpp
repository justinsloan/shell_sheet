// Regression test for the Gtk::TextMark gravity bug found while building
// the interactive-stdin feature (see shell_sheet.cpp's on_key_press() and
// flush_output_buffer() for the real usage this pins down).
//
// Unlike tests/test_syntax_highlight.cpp, this needs a real Gtk::TextBuffer
// (creating one requires GTK to be initialized against a display), so it
// needs DISPLAY set to something usable - a real X/Wayland session, or
// Xvfb. Run via `make test-marks`, not part of the default `make test`.
//
// The bug: a Gtk::TextMark used to track "where the user's pending,
// not-yet-submitted input starts" was given the same right-gravity as the
// mark used for "where output gets appended". Right gravity is correct for
// the output mark (it must advance past each newly-inserted chunk of
// output, so later output keeps appending after earlier output, in order).
// It's wrong for the input-boundary mark: with right gravity, the user's
// own first keystroke - inserted exactly at that mark's position - drags
// the mark forward right along with it, so by the time Enter is pressed,
// the mark has already caught up to the end of the buffer and the
// "captured" line comes out empty every time.
#include <gtkmm.h>

#include <cassert>
#include <iostream>

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    std::cout << (cond ? "PASS: " : "FAIL: ") << what << "\n";
    if (!cond) ++g_failures;
}

} // namespace

int main() {
    int argc = 0;
    char** argv = nullptr;
    Gtk::Main kit(argc, argv); // initializes GTK synchronously; no main loop needed below

    auto buffer = Gtk::TextBuffer::create();

    // --- Isolated gravity check: does GTK behave the way we're relying on? ---
    auto right_gravity_mark = buffer->create_mark("right", buffer->end(), false);
    auto left_gravity_mark = buffer->create_mark("left", buffer->end(), true);

    buffer->insert(buffer->end(), "hello");
    // Both marks started at the same position (buffer start, which was also
    // the end of an empty buffer) and "hello" was inserted exactly there.
    expect(right_gravity_mark->get_iter().get_offset() == 5,
           "right-gravity mark advances past text inserted at its position");
    expect(left_gravity_mark->get_iter().get_offset() == 0,
           "left-gravity mark stays put when text is inserted at its position");

    // --- The actual bug scenario: output flush, then simulated typing ---
    buffer->set_text("");
    auto output_mark = buffer->create_mark("command_mark", buffer->end(), false);
    auto input_mark = buffer->create_mark("input_mark", buffer->end(), true);

    // Simulate flush_output_buffer(): insert output at output_mark, then
    // resync input_mark to output_mark's new (advanced) position - this is
    // the explicit resync shell_sheet.cpp performs, not something left to
    // gravity alone.
    buffer->insert(output_mark->get_iter(), "Continue? [Y/n] ");
    buffer->move_mark(input_mark, output_mark->get_iter());

    // Simulate the user typing "y" - GTK's default TextView handling would
    // insert it at the cursor, which at this point coincides with
    // input_mark's position.
    buffer->insert(buffer->end(), "y");

    // This is the exact call on_key_press() makes to capture what was
    // typed. If input_mark had wrongly advanced along with "y" (as it would
    // with right gravity), this comes back empty instead of "y".
    std::string captured = buffer->get_text(input_mark->get_iter(), buffer->end());
    expect(captured == "y", "pending input is correctly captured after simulated typing");

    // Simulate on_key_press() submitting the line: both marks move to the
    // new end, ready for the next round.
    buffer->insert(buffer->end(), "\n");
    buffer->move_mark(output_mark, buffer->end());
    buffer->move_mark(input_mark, buffer->end());

    expect(buffer->get_text(input_mark->get_iter(), buffer->end()).empty(),
           "pending input is empty again immediately after submitting a line");

    // A second round, to make sure the boundary genuinely reset rather than
    // happening to work once.
    buffer->insert(buffer->end(), "n");
    captured = buffer->get_text(input_mark->get_iter(), buffer->end());
    expect(captured == "n", "pending input capture still works correctly on a second round");

    // --- forward_to_line_end()'s "already at a line end" trap ---
    //
    // Pins down the GTK semantic that ShellSheet::snap_range_to_lines()
    // guards against. forward_to_line_end() on an iterator that is ALREADY
    // sitting at a line end does not stay put - it moves to the end of the
    // NEXT line. The original snap-to-lines code only checked
    // get_line_offset() != 0, so selecting exactly one whole line (no
    // trailing newline) and running Sort Lines silently pulled the
    // following line into the sorted range and rewrote it.
    auto snap_buf = Gtk::TextBuffer::create();
    snap_buf->set_text("aaa\nbbb\nccc\n");

    Gtk::TextIter line_end = snap_buf->get_iter_at_line(1);
    line_end.forward_to_line_end();
    expect(line_end.get_line() == 1 && line_end.ends_line(),
           "forward_to_line_end from a line start lands at that line's end");

    Gtk::TextIter moved_again = line_end;
    moved_again.forward_to_line_end();
    expect(moved_again.get_line() == 2,
           "forward_to_line_end from a line END jumps to the NEXT line's end");

    // The guard itself: snapping a range that already ends at a line end
    // must leave it exactly where it is.
    Gtk::TextIter snap_start = snap_buf->get_iter_at_line(1);
    Gtk::TextIter snap_end = line_end;
    snap_start.set_line_offset(0);
    if (snap_end.get_line_offset() != 0 && !snap_end.ends_line()) {
        snap_end.forward_to_line_end();
    }
    expect(snap_buf->get_text(snap_start, snap_end) == "bbb",
           "the guarded snap keeps a whole-line selection to just that line");

    // --- the line-number gutter's font mechanism ---
    //
    // The gutter draws its numbers with a Pango layout using the font
    // description it reads back off the TextView's style context. That
    // indirection is the whole reason the numbers track the editor text
    // when the zoom changes, so pin it down: change the CSS font size, and
    // a layout built from the resolved description must change size too.
    Gtk::Window font_win;
    Gtk::TextView font_tv;
    font_win.add(font_tv);
    font_win.show_all();

    auto font_css = Gtk::CssProvider::create();
    Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), font_css,
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    auto width_at = [&](int points) {
        font_css->load_from_data("textview { font-family: monospace; font-size: " +
                                  std::to_string(points) + "pt; }");
        auto layout = font_tv.create_pango_layout("1234567890");
        layout->set_font_description(
            font_tv.get_style_context()->get_font(Gtk::STATE_FLAG_NORMAL));
        int w = 0, h = 0;
        layout->get_pixel_size(w, h);
        return w;
    };

    int small = width_at(9);
    int medium = width_at(16);
    int large = width_at(28);
    expect(small < medium && medium < large,
           "line numbers measure wider as the editor font size grows");
    expect(width_at(9) == small,
           "...and measure back to the same width on the way down");

    Gtk::StyleContext::remove_provider_for_screen(Gdk::Screen::get_default(), font_css);

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All mark-tracking tests passed\n";
    return 0;
}
