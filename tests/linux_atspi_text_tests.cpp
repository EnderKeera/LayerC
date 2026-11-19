#include "linux_atspi_interfaces_fixture.hpp"

#include <utility>

namespace layerc::test::atspi_fixture {

namespace {

constexpr const char *kTextInterface = "org.a11y.atspi.Text";
constexpr const char *kEditableInterface = "org.a11y.atspi.EditableText";

CallResult text_call(Harness &harness, const std::string &path,
                     const char *interface, const std::string &member,
                     const std::function<int(sd_bus_message *)> &build) {
  return harness.ask(harness.embed_name(), path, interface, member, build);
}

std::optional<std::string> node_text(Harness &harness, const std::string &path) {
  return reply_text(text_call(harness, path, kTextInterface, "GetText",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "ii", 0, -1);
                              })
                        .reply);
}

std::optional<std::int32_t> character_count(Harness &harness,
                                            const std::string &path) {
  const auto result = harness.property(path, kTextInterface, "CharacterCount");
  return variant_int(result.reply);
}

std::optional<std::int32_t> caret_offset(Harness &harness,
                                         const std::string &path) {
  const auto result = harness.property(path, kTextInterface, "CaretOffset");
  return variant_int(result.reply);
}

std::optional<TextSpan> span_at(Harness &harness, const std::string &path,
                                const char *member, const std::int32_t offset,
                                const std::uint32_t unit) {
  return parse_span(text_call(harness, path, kTextInterface, member,
                              [&](sd_bus_message *call) {
                                return sd_bus_message_append(call, "iu", offset,
                                                             unit);
                              })
                        .reply);
}

// The attribute set a Text answer carries; the model publishes none.
std::size_t attribute_count(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{ss}") < 0)
    return 0;
  std::size_t count = 0;
  while (sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "ss") >
         0) {
    ++count;
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  return count;
}

} // namespace

// --- case d: Text ---------------------------------------------------------

void text_offsets_are_characters_and_geometry_is_real() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto path = node_path_named(harness, "Name");
  if (!expect(path.has_value(), "the field has a path")) return;
  const std::string field = *path;

  // "aé😀 b\nline2": eleven characters, thirteen bytes.
  expect(character_count(harness, field) == std::optional<std::int32_t>{11},
         "offsets count characters, not bytes");
  expect(node_text(harness, field) ==
             std::optional<std::string>{"a\xc3\xa9\xf0\x9f\x98\x80 b\nline2"},
         "GetText answers the whole field");
  const auto range = [&](const std::int32_t start, const std::int32_t end) {
    return reply_text(text_call(harness, field, kTextInterface, "GetText",
                                [&](sd_bus_message *call) {
                                  return sd_bus_message_append(call, "ii", start,
                                                               end);
                                })
                          .reply);
  };
  expect(range(0, 2) == std::optional<std::string>{"a\xc3\xa9"},
         "a two character prefix never splits a code point");
  expect(range(2, 3) == std::optional<std::string>{"\xf0\x9f\x98\x80"},
         "one character is one code point");
  expect(range(3, -1) == std::optional<std::string>{" b\nline2"},
         "an end of -1 means the end of the text");
  expect(range(9, 5) == std::optional<std::string>{""},
         "a reversed range is empty");
  expect(range(-1, 3) == std::optional<std::string>{""},
         "a negative start is empty");
  expect(range(12, -1) == std::optional<std::string>{""},
         "a start past the end is empty");
  expect(range(11, 11) == std::optional<std::string>{""},
         "a collapsed range is empty");

  const auto codepoint = [&](const std::int32_t offset) {
    return reply_i32(text_call(harness, field, kTextInterface,
                               "GetCharacterAtOffset",
                               [&](sd_bus_message *call) {
                                 return sd_bus_message_append(call, "i", offset);
                               })
                         .reply);
  };
  expect(codepoint(0) == std::optional<std::int32_t>{'a'},
         "the first character is its code point");
  expect(codepoint(2) == std::optional<std::int32_t>{0x1F600},
         "an astral character decodes to one code point");
  expect(codepoint(11) == std::optional<std::int32_t>{0},
         "an offset past the end carries no character");

  // AtspiTextGranularity: 0 character, 1 word, 2 sentence, 3 line, 4 paragraph.
  const auto word = span_at(harness, field, "GetStringAtOffset", 0, 1);
  expect(word && word->text == "a\xc3\xa9\xf0\x9f\x98\x80" && word->start == 0 &&
             word->end == 3,
         "a word run spells the characters up to the space");
  const auto second_word = span_at(harness, field, "GetStringAtOffset", 4, 1);
  expect(second_word && second_word->text == "b" && second_word->start == 4,
         "the next word starts after the space");
  const auto line = span_at(harness, field, "GetStringAtOffset", 6, 3);
  expect(line && line->text == "line2" && line->start == 6 && line->end == 11,
         "a line run stops at the newline");
  const auto sentence = span_at(harness, field, "GetStringAtOffset", 0, 2);
  expect(sentence && sentence->text.size() > 0 && sentence->start == 0,
         "a sentence run starts at the offset");

  // AtspiTextBoundaryType: 0 char, 1 word start, 2 word end, 3 sentence start,
  // 4 sentence end, 5 line start, 6 line end.
  const auto first_line = span_at(harness, field, "GetTextAtOffset", 0, 5);
  expect(first_line && first_line->text == "a\xc3\xa9\xf0\x9f\x98\x80 b" &&
             first_line->start == 0 && first_line->end == 5,
         "GetTextAtOffset names the line around the offset");
  const auto before = span_at(harness, field, "GetTextBeforeOffset", 6, 5);
  expect(before && before->text == "a\xc3\xa9\xf0\x9f\x98\x80 b" &&
             before->end == 5,
         "GetTextBeforeOffset names the line in front of a newline");
  const auto after = span_at(harness, field, "GetTextAfterOffset", 0, 1);
  expect(after && after->text == " " && after->start == 3,
         "GetTextAfterOffset names the run behind the word");

  // Attributes are not in the model, so every attribute answer is empty and
  // the run covers the whole text.
  const auto read_attributes = [&] {
    const auto call = text_call(harness, field, kTextInterface, "GetAttributes",
                                [](sd_bus_message *call) {
                                  return sd_bus_message_append(call, "i", 0);
                                });
    return attribute_count(call.reply);
  };
  expect(read_attributes() == 0, "the field publishes no attributes");
  const auto defaults = text_call(harness, field, kTextInterface,
                                  "GetDefaultAttributes", {});
  expect(attribute_count(defaults.reply) == 0,
         "the field has no default attributes");

  // Caret and selection are characters.
  expect(parse_bool(text_call(harness, field, kTextInterface, "SetCaretOffset",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "i", 4);
                              })
                        .reply) == std::optional<bool>{true},
         "a caret offset is accepted");
  harness.spin(ms(250));
  expect(caret_offset(harness, field) == std::optional<std::int32_t>{4},
         "the caret reports the character offset it took");
  expect(fixture.name->insert_position() == 8,
         "the widget's caret sits after the fourth character");

  expect(parse_bool(text_call(harness, field, kTextInterface, "AddSelection",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "ii", 1, 3);
                              })
                        .reply) == std::optional<bool>{true},
         "a character range is selected");
  harness.spin(ms(250));
  {
    const auto caret = harness.property(field, kTextInterface, "CaretOffset");
    expect(variant_int(caret.reply) == std::optional<std::int32_t>{3},
           "selecting moves the caret to the range end");
  }
  expect(reply_i32(harness.get(field, kTextInterface, "GetNSelections").reply) ==
             std::optional<std::int32_t>{1},
         "the field has one selection");
  const auto chosen = parse_pair(
      text_call(harness, field, kTextInterface, "GetSelection",
                [](sd_bus_message *call) {
                  return sd_bus_message_append(call, "i", 0);
                })
          .reply);
  expect(chosen == std::optional<std::pair<std::int32_t, std::int32_t>>{
                        std::pair<std::int32_t, std::int32_t>{1, 3}},
         "the selection reports its character range");
  expect(parse_bool(text_call(harness, field, kTextInterface, "RemoveSelection",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "i", 0);
                              })
                        .reply) == std::optional<bool>{true},
         "removing the selection is accepted");
  harness.spin(ms(250));
  expect(reply_i32(harness.get(field, kTextInterface, "GetNSelections").reply) ==
             std::optional<std::int32_t>{0},
         "a collapsed selection holds nothing");

  // Glyph geometry: the first character sits inside the field, and each
  // coordinate space offsets it by the window origin.
  const auto extents = [&](const std::uint32_t coordinate) {
    return parse_rect(
        text_call(harness, field, kTextInterface, "GetCharacterExtents",
                  [&](sd_bus_message *call) {
                    return sd_bus_message_append(call, "iu", 0, coordinate);
                  })
            .reply, "iiii");
  };
  const auto screen_rect = extents(0);
  const auto window_rect = extents(1);
  if (!expect(screen_rect && window_rect, "the field publishes glyph geometry"))
    return;
  expect(screen_rect->width > 0.0 && screen_rect->height > 0.0,
         "a character has real extent");
  expect(window_rect->left != screen_rect->left ||
             window_rect->top != screen_rect->top,
         "window coordinates are relative to the toplevel");

  const auto hit = reply_i32(
      text_call(harness, field, kTextInterface, "GetOffsetAtPoint",
                [&](sd_bus_message *call) {
                  return sd_bus_message_append(
                      call, "iiu",
                      static_cast<std::int32_t>(screen_rect->left + 1),
                      static_cast<std::int32_t>(screen_rect->top + 1), 0U);
                })
          .reply);
  expect(hit == std::optional<std::int32_t>{0},
         "a point inside the first character answers its offset");
  const auto outside = reply_i32(
      text_call(harness, field, kTextInterface, "GetOffsetAtPoint",
                [](sd_bus_message *call) {
                  return sd_bus_message_append(call, "iiu", -4000, -4000, 0U);
                })
          .reply);
  expect(outside == std::optional<std::int32_t>{-1},
         "a point outside the text answers no offset");

  const auto range_rect = parse_rect(
      text_call(harness, field, kTextInterface, "GetRangeExtents",
                [](sd_bus_message *call) {
                  return sd_bus_message_append(call, "iiu", 0, 3, 0U);
                })
          .reply, "iiii");
  expect(range_rect && range_rect->width > screen_rect->width,
         "a range is wider than one character");

  // A masked field publishes no text at all, and a read-only one publishes
  // text but no edits.
  const auto masked = node_path_named(harness, "Passcode");
  if (!expect(masked.has_value(), "the masked field has a path")) return;
  expect(harness.get(*masked, kTextInterface, "GetText").failed,
         "a masked field carries no text interface");
  const auto reference_path = node_path_named(harness, "Reference");
  if (!expect(reference_path.has_value(), "the read-only field has a path"))
    return;
  expect(node_text(harness, *reference_path) ==
             std::optional<std::string>{"fixed"},
         "a read-only field still publishes its text");
  expect(harness
             .ask(harness.embed_name(), *reference_path, kEditableInterface,
                  "SetTextContents",
                  [](sd_bus_message *call) {
                    return sd_bus_message_append(call, "s", "other");
                  })
             .failed,
         "a read-only field refuses edits");
  expect(fixture.reference->value() == std::optional<std::string>{"fixed"},
         "the read-only field kept its text");
}

// --- case e: EditableText -------------------------------------------------

void edits_run_through_the_fields_own_path_and_report_text_changes() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:text-changed"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto path = node_path_named(harness, "Name");
  if (!expect(path.has_value(), "the field has a path")) return;
  const std::string field = *path;
  const EventQuery query{"org.a11y.atspi.Event.Object", "TextChanged", ""};
  const auto text_change = [&](const std::size_t mark,
                               const std::string &change) {
    (void)wait_event(harness, mark, EventQuery{query.interface, query.member,
                                               change},
                     ms(3000));
    const auto &signals = harness.signals();
    for (std::size_t i = mark; i < signals.size(); ++i) {
      const auto &signal = signals[i];
      if (signal.interface == query.interface && signal.member == query.member &&
          signal.path == field && signal.name == change)
        return true;
    }
    return false;
  };

  std::size_t mark = harness.signals().size();
  expect(parse_bool(text_call(harness, field, kEditableInterface,
                              "SetTextContents",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "s", "fresh");
                              })
                        .reply) == std::optional<bool>{true},
         "a client replaces the whole text");
  expect(harness.spin_until(
             [&] { return fixture.name->value() != nullptr &&
                          std::string{fixture.name->value()} == "fresh"; },
             ms(2000)),
         "the widget holds the replacement text");
  expect(text_change(mark, "delete") || text_change(mark, "insert"),
         "the replacement reports a text change");

  mark = harness.signals().size();
  expect(parse_bool(text_call(harness, field, kEditableInterface, "InsertText",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "isi", 0,
                                                             "\xc3\xa9" "tail", 2);
                              })
                        .reply) == std::optional<bool>{true},
         "an insertion at the start is accepted");
  expect(harness.spin_until(
             [&] { return fixture.name->value() != nullptr &&
                          std::string{fixture.name->value()} ==
                              "\xc3\xa9"
                              "fresh"; },
             ms(2000)),
         "the specified byte prefix inserts one complete character and no suffix");
  expect(text_change(mark, "insert"), "an insertion reports a text change");
  expect(caret_offset(harness, field) == std::optional<std::int32_t>{1},
         "the caret follows the inserted character");

  mark = harness.signals().size();
  expect(parse_bool(text_call(harness, field, kEditableInterface, "DeleteText",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "ii", 0, 1);
                              })
                        .reply) == std::optional<bool>{true},
         "a deletion of the first character is accepted");
  expect(harness.spin_until(
             [&] { return fixture.name->value() != nullptr &&
                          std::string{fixture.name->value()} == "fresh"; },
             ms(2000)),
         "the widget lost the deleted character");
  expect(text_change(mark, "delete"), "a deletion reports a text change");

  expect(text_call(harness, field, kEditableInterface, "InsertText",
                   [](sd_bus_message *call) {
                     return sd_bus_message_append(call, "isi", 99, "x", 1);
                   })
             .failed,
          "an insertion past the end is refused");
  const auto split = text_call(harness, field, kEditableInterface, "InsertText",
                               [](sd_bus_message *call) {
                                 return sd_bus_message_append(call, "isi", 0, "\xc3\xa9", 1);
                               });
  expect(parse_bool(split.reply) == std::optional<bool>{false},
          "a length splitting a multibyte character is refused");
  expect(text_call(harness, field, kEditableInterface, "InsertText",
                   [](sd_bus_message *call) {
                     return sd_bus_message_append(call, "isi", 0, "x", -1);
                   }).failed,
          "a negative insertion length is refused");
  expect(node_text(harness, field) == std::optional<std::string>{"fresh"},
          "invalid insertion lengths leave the text unchanged");
  const auto deleted = text_call(harness, field, kEditableInterface,
                                 "DeleteText",
                                 [](sd_bus_message *call) {
                                   return sd_bus_message_append(call, "ii", 3, 1);
                                 });
  expect(deleted.failed || parse_bool(deleted.reply) == std::optional<bool>{false},
         "a reversed deletion is refused");

  // The deprecated clipboard routes stay unimplemented, as they were for the
  // ATK interface this replaces.
  const auto copy = text_call(harness, field, kEditableInterface, "CopyText",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "ii", 0, 2);
                              });
  expect(copy.failed && copy.error == SD_BUS_ERROR_NOT_SUPPORTED,
         "copy is explicitly unsupported, not a private boolean reply");
  const auto paste = text_call(harness, field, kEditableInterface, "PasteText",
                               [](sd_bus_message *call) {
                                 return sd_bus_message_append(call, "i", 0);
                               });
  expect(parse_bool(paste.reply) == std::optional<bool>{false},
         "paste is not an editor route");

  // A multi-line field edits across its newline.
  const auto notes = node_path_named(harness, "Notes");
  if (!expect(notes.has_value(), "the multi-line field has a path")) return;
  expect(parse_bool(text_call(harness, *notes, kEditableInterface, "InsertText",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "isi", 12,
                                                             "\nthird", 20);
                              })
                        .reply) == std::optional<bool>{true},
         "a multi-line field takes an insertion at its end");
  expect(harness.spin_until(
             [&] {
               return fixture.notes->value() != nullptr &&
                      std::string{fixture.notes->value()} == "first\nsecond\nthird";
             },
             ms(2000)),
         "the multi-line field grew a line");
  expect(node_text(harness, *notes) ==
             std::optional<std::string>{"first\nsecond\nthird"},
         "the published text matches the widget");
}

// --- case f: the registry's spelling of a registration --------------------

void a_registry_spelled_registration_reaches_the_text_events() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  // What at-spi2-core answers after rewriting a registration, not what the
  // client sent.
  harness.set_registered_listeners({{":1.50", "Object:TextChanged:"},
                                    {":1.50", "Object:TextCaretMoved:"}});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto path = node_path_named(harness, "Name");
  if (!expect(path.has_value(), "the field has a path")) return;
  const std::string field = *path;

  const auto saw = [&](const std::size_t mark, const char *member,
                       const std::string &name) {
    const auto &signals = harness.signals();
    for (std::size_t i = mark; i < signals.size(); ++i) {
      const auto &signal = signals[i];
      if (signal.interface == "org.a11y.atspi.Event.Object" &&
          signal.member == member && signal.path == field &&
          signal.name == name)
        return true;
    }
    return false;
  };

  std::size_t mark = harness.signals().size();
  expect(parse_bool(text_call(harness, field, kEditableInterface,
                              "SetTextContents",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "s", "fresh");
                              })
                        .reply) == std::optional<bool>{true},
         "a client replaces the whole text");
  expect(harness.spin_until(
             [&] {
               return saw(mark, "TextChanged", "insert") ||
                      saw(mark, "TextChanged", "delete");
             },
             ms(3000)),
         "a registry-spelled text-changed registration receives the change");

  mark = harness.signals().size();
  expect(parse_bool(text_call(harness, field, kTextInterface, "SetCaretOffset",
                              [](sd_bus_message *call) {
                                return sd_bus_message_append(call, "i", 2);
                              })
                        .reply) == std::optional<bool>{true},
         "a client moves the caret");
  expect(harness.spin_until([&] { return saw(mark, "TextCaretMoved", {}); },
                            ms(3000)),
         "a registry-spelled caret registration receives the move");
}

} // namespace layerc::test::atspi_fixture
