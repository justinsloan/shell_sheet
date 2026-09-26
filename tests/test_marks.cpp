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
#include <gtkmm/init.h>

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
    // GTK4 removed Gtk::Main. gtk_init() brings up the C library, but the
    // gtkmm C++ wrappers need registering separately - without that second
    // call, wrapping a GtkTextMark fails and the process segfaults. No main
    // loop is needed for any of the checks below.
    gtk_init();
    Gtk::init_gtkmm_internals();

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
    // The gutter draws its numbers with a Pango layout built from the same
    // family and size that the editor's CSS uses, which is what keeps the
    // two in step when the zoom changes. Under GTK3 this test read the font
    // back off the TextView's style context; GTK4 removed
    // StyleContext::get_font(), and ShellSheet::text_view_font() now builds
    // the description directly - so this mirrors that, and still pins down
    // the property that matters: a bigger size must measure wider.
    Gtk::TextView font_tv;

    auto width_at = [&](int points) {
        Pango::FontDescription font("monospace");
        font.set_size(points * PANGO_SCALE);
        auto layout = font_tv.create_pango_layout("1234567890");
        layout->set_font_description(font);
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

    if (g_failures > 0) {
        std::cout << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All mark-tracking tests passed\n";
    return 0;
}
