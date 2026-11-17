#pragma once

#include "linux_atspi_test_harness.hpp"

namespace layerc::test::atspi_fixture {

// --- the editor under test ------------------------------------------------

struct Fixture {
  Fl_Window *window{};
  Fl_Button *button{};
  Fl_Input *input{};
  Fl_Check_Button *check{};
  Fl_Browser *browser{};
  FltkTree tree;

  Fixture() {
    window = new Fl_Window(40, 40, 420, 300, "LayerC Editor");
    button = new Fl_Button(20, 20, 120, 28, "Run");
    input = new Fl_Input(20, 60, 200, 28, "Name");
    check = new Fl_Check_Button(20, 100, 160, 28, "Enabled");
    browser = new Fl_Browser(20, 140, 200, 120);
    browser->add("alpha");
    browser->add("beta");
    window->end();
    window->show();
    Fl::wait(0.01);
  }

  ~Fixture() {
    Fl::delete_widget(window);
    Fl::wait(0.01);
  }

  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
};

// --- event queries --------------------------------------------------------

struct EventQuery {
  std::string interface, member, name;
};

[[nodiscard]] std::optional<Signal> find_event(const Harness &harness,
                                              std::size_t mark,
                                              const EventQuery &query);
bool wait_event(Harness &harness, std::size_t mark, const EventQuery &query,
                std::chrono::milliseconds limit);

} // namespace layerc::test::atspi_fixture
