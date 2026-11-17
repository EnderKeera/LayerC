#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace layerc::editor::a11y {

using NodeId = std::uint64_t;
inline constexpr NodeId no_node = 0;

enum class Role {
  application, window, dialog, button, check_box, radio_button, text_field,
  number_field, drop_down, list, list_row, menu_bar, menu, menu_item,
  check_menu_item, radio_menu_item, tab_list, tab, scroll_bar, slider, scroll_pane,
  canvas, label, status, image, group
};

struct Bounds {
  double left{}, top{}, width{}, height{};
};

struct States {
  bool focusable{}, focused{}, selected{}, checked{}, mixed{};
  bool expanded{}, collapsed{}, disabled{}, read_only{}, modal{}, showing{true};
  bool password{};
  bool visible{true}, active{}, resizable{}, editable{}, multi_line{};
  bool checkable{}, selectable{};
};

struct TextRun {
  std::size_t begin{}, end{};
  Bounds bounds;
};

struct Text {
  std::string value;
  // UTF-8 byte offsets, matching FLTK. Adaptors translate to their protocols'
  // units. Every offset is validated at a code-point boundary before publish.
  std::size_t caret{}, anchor{};
  std::vector<TextRun> runs{};
  Bounds caret_bounds{};
};

struct Range {
  double minimum{}, maximum{}, step{}, value{};
};

struct Node {
  NodeId id{}, parent{};
  Role role{Role::group};
  std::string name, description, value;
  States states;
  Bounds bounds;
  std::optional<Text> text;
  std::optional<Range> range;
  // Protocol-neutral keybinding (mnemonic and accelerator) and action names;
  // an empty action list means the node exposes no action interface.
  std::string keybinding;
  std::vector<std::string> actions;
  std::vector<NodeId> children, labelled_by, described_by;
};

struct Snapshot {
  NodeId root{};
  std::map<NodeId, Node> nodes;
  std::string announcement;
  // Monotonic announcement occurrence. Repeating the same text is a new
  // occurrence, so adaptors never rely on the text alone to detect one.
  std::uint64_t announcement_sequence{};
  // Node an announcement is raised on: the source widget's node, else the
  // active window's, else the root.
  NodeId announcement_source{};
  std::uint64_t generation{};
};

struct ModelError {
  NodeId node{};
  std::string message;
};

// Providers may run on platform-owned RPC threads. Neither an adaptor nor a
// client receives references into mutable widget/model state.
class Model {
public:
  [[nodiscard]] std::optional<ModelError> publish(Snapshot snapshot);
  [[nodiscard]] Snapshot read() const;
  [[nodiscard]] std::optional<Node> read(NodeId id) const;
  void clear();
private:
  mutable std::mutex mutex_;
  Snapshot snapshot_;
};

enum class ActionKind { focus, invoke, set_value, select_text, set_number };
struct Action {
  NodeId node{};
  ActionKind kind{ActionKind::focus};
  std::string value{};
  // UTF-8 byte offsets for select_text and for set_value caret placement.
  std::size_t caret{}, anchor{};
  // set_value moves the caret when the protocol action does, and only then.
  bool place_caret{};
  double number{};
};

} // namespace layerc::editor::a11y
