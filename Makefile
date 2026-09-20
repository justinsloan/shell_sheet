CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 $(shell pkg-config --cflags gtkmm-3.0)
LDFLAGS = $(shell pkg-config --libs gtkmm-3.0)
TARGET = shell_sheet
SRCS = main.cpp shell_sheet.cpp syntax_highlight.cpp text_search.cpp text_transforms.cpp undo_stack.cpp directory_tracking.cpp
OBJS = $(SRCS:.cpp=.o)
TEST_TARGET = tests/test_syntax_highlight
TRANSFORMS_TEST_TARGET = tests/test_text_transforms
SEARCH_TEST_TARGET = tests/test_text_search
UNDO_TEST_TARGET = tests/test_undo_stack
DIRTRACK_TEST_TARGET = tests/test_directory_tracking
MARKS_TEST_TARGET = tests/test_marks

all: $(TARGET)

rebuild: clean all

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.cpp shell_sheet.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# GTK-free unit tests for the pure syntax-highlighting and text-transform
# logic. Built without gtkmm/pkg-config flags on purpose, since neither
# syntax_highlight.cpp nor text_transforms.cpp has any GTK dependency -
# that separation is what makes them unit-testable at all.
#
# test_text_search is the one exception: it needs glibmm (Glib::Regex), but
# still no GTK widgets or display, so it stays just as fast/headless-safe
# and is bundled in here rather than alongside the display-requiring
# test-marks target below.
test: $(TEST_TARGET) $(TRANSFORMS_TEST_TARGET) $(SEARCH_TEST_TARGET) $(UNDO_TEST_TARGET) $(DIRTRACK_TEST_TARGET)
	./$(TEST_TARGET)
	./$(TRANSFORMS_TEST_TARGET)
	./$(SEARCH_TEST_TARGET)
	./$(UNDO_TEST_TARGET)
	./$(DIRTRACK_TEST_TARGET)

$(TEST_TARGET): tests/test_syntax_highlight.cpp syntax_highlight.cpp syntax_highlight.h
	$(CXX) -Wall -Wextra -std=c++17 tests/test_syntax_highlight.cpp syntax_highlight.cpp -o $(TEST_TARGET)

$(TRANSFORMS_TEST_TARGET): tests/test_text_transforms.cpp text_transforms.cpp text_transforms.h
	$(CXX) -Wall -Wextra -std=c++17 tests/test_text_transforms.cpp text_transforms.cpp -o $(TRANSFORMS_TEST_TARGET)

$(SEARCH_TEST_TARGET): tests/test_text_search.cpp text_search.cpp text_search.h
	$(CXX) -Wall -Wextra -std=c++17 $(shell pkg-config --cflags glibmm-2.4) tests/test_text_search.cpp text_search.cpp -o $(SEARCH_TEST_TARGET) $(shell pkg-config --libs glibmm-2.4)

$(UNDO_TEST_TARGET): tests/test_undo_stack.cpp undo_stack.cpp undo_stack.h
	$(CXX) -Wall -Wextra -std=c++17 tests/test_undo_stack.cpp undo_stack.cpp -o $(UNDO_TEST_TARGET)

$(DIRTRACK_TEST_TARGET): tests/test_directory_tracking.cpp directory_tracking.cpp directory_tracking.h
	$(CXX) -Wall -Wextra -std=c++17 tests/test_directory_tracking.cpp directory_tracking.cpp -o $(DIRTRACK_TEST_TARGET)

# Regression test for the Gtk::TextMark gravity bug (see tests/test_marks.cpp).
# Unlike `test` above, this genuinely needs GTK and a display (a real
# session, or Xvfb) - it's checking real Gtk::TextBuffer/TextMark behavior,
# not pure logic, so kept as a separate target rather than folded into the
# default `test`, which stays fast and portable.
test-marks: $(MARKS_TEST_TARGET)
	./$(MARKS_TEST_TARGET)

$(MARKS_TEST_TARGET): tests/test_marks.cpp
	$(CXX) $(CXXFLAGS) tests/test_marks.cpp -o $(MARKS_TEST_TARGET) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_TARGET) $(TRANSFORMS_TEST_TARGET) $(SEARCH_TEST_TARGET) $(UNDO_TEST_TARGET) $(DIRTRACK_TEST_TARGET) $(MARKS_TEST_TARGET)

.PHONY: all rebuild clean test test-marks