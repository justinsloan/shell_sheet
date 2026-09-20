# shell_sheet

A text editor where any line can be a shell command.

Put the cursor on a line, press **Ctrl+Enter**, and the command runs — its
output lands in the document right underneath it. The whole session stays in
one editable buffer you can search, transform and save, which makes it a
useful place to work out a sequence of commands, keep the output alongside
them, and hand the result to someone else.

It's modelled on BBEdit's Unix Worksheet. Written in C++17 and gtkmm-3.

```
#!/bin/bash
cd /var/log
ls -la *.log
total 248
-rw-r-----  1 syslog adm  18229 Sep 19 17:04 syslog
-rw-r-----  1 syslog adm   4021 Sep 19 16:58 auth.log
```

## What it does

**Runs commands where you write them.** Output is appended below the command
line. Commands that ask questions work too — type your answer and press Enter
and it goes to the command's stdin, so `apt`'s `Do you want to continue? [Y/n]`
behaves as you'd expect. `sudo` prompts for a password in a dialog, and the password goes
straight from that dialog to `sudo` without passing through the editor
process, so sudo's own credential cache still applies.

**Knows which programs it can't host.** A full-screen curses program — `vim`,
`htop`, `mc`, `watch` — can't live in a text buffer, so those open in a real
terminal window instead. The test is "does it need a TTY", not "does it run
forever": `ping` runs indefinitely and still streams into the buffer as usual.
Anything else can be forced into a terminal with **Ctrl+Shift+Enter**.

**`cd` sticks.** Each command is its own shell, so a `cd` inside one would
normally die with it. `cd` lines are handled by the app instead and apply to
every command after them. The current directory shows in a **pwd** bar
(View ▸ Show Working Directory).

**Tells you when it's busy.** An orange bar names the running command, and
Ctrl+C stops it — leaving `Terminated` in the transcript, the way a shell
would.

Also: shell syntax highlighting, find & replace with regex, full undo/redo,
zoom, soft wrap, and a Text menu of transformations (case, sort, indent,
duplicate/move lines, hard wrap, tabs↔spaces, line endings, trim).

## Building

Needs a C++17 compiler and gtkmm-3.0.

```bash
sudo apt install build-essential libgtkmm-3.0-dev   # Debian / Ubuntu
sudo dnf install gcc-c++ gtkmm30-devel              # Fedora

make
./shell_sheet
```

Builds with `-Wall -Wextra` and is expected to stay warning-free.

## Tests

```bash
make test        # pure-logic unit tests, no display needed
make test-marks  # GTK behaviour; needs a display (a real one, or Xvfb)
```

The logic that can be separated from the GUI lives in its own modules —
`text_search`, `text_transforms`, `undo_stack`, `syntax_highlight`,
`directory_tracking`, `terminal_launch` — each with a test binary, so most
behaviour is testable without a display. `test-marks` covers the parts that
genuinely need GTK, such as `TextMark` gravity and font metrics.

## Keyboard

| | |
|---|---|
| **Ctrl+Enter** | Run the current line (or selection) |
| **Ctrl+Shift+Enter** | Run it in a terminal window |
| **Ctrl+C** | Stop the running command — otherwise, copy |
| **Ctrl+D** | Send EOF to it |
| Ctrl+O / Ctrl+S / Ctrl+Q | Open / Save / Quit |
| Ctrl+Z / Ctrl+Shift+Z | Undo / Redo |
| Ctrl+F / Ctrl+G / Ctrl+Shift+G | Find / Next / Previous |
| Ctrl+H | Replace |
| Esc | Close the search bar |
| Ctrl+L | Select line |
| Tab / Shift+Tab | Indent / dedent selected lines |
| Ctrl+Shift+D | Duplicate line |
| Alt+Up / Alt+Down | Move line or selection |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |

## Known limitations

- Only the first command on a line is examined when deciding whether a
  program needs a terminal or whether a `cd` should be tracked, so
  `cd /tmp && vim x` does neither. This is deliberate — guessing at shell
  grammar would be worse than not trying.
- Search matches within a line, never across a line break.
- Byte and character offsets are treated as the same thing, which is correct
  for ASCII and wrong for multi-byte UTF-8.
- A multi-line paste isn't forwarded to a running command line by line.

## Licence

No licence has been chosen yet — all rights reserved by default until one is
added.
