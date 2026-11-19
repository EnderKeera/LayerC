// Prefix advance cache for the exported text geometry of FLTK input fields.
#ifndef LAYERC_EDITOR_FLTK_A11Y_TEXT_METRICS_HPP
#define LAYERC_EDITOR_FLTK_A11Y_TEXT_METRICS_HPP

#include <FL/Fl_Input_.H>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace layerc::editor::a11y {
// Caches the advances that field_text derives from fl_width. An advance is a
// pure function of the font (its id, face name and size, and the driver's
// scale) and of the expanded line bytes up to one glyph, so the cache never
// keys on widget state such as scroll offsets and needs no invalidation.
class TextMetrics {
public:
  TextMetrics();
  ~TextMetrics();
  TextMetrics(TextMetrics &&) noexcept;
  TextMetrics &operator=(TextMetrics &&) noexcept;
  TextMetrics(const TextMetrics &) = delete;
  TextMetrics &operator=(const TextMetrics &) = delete;

  // Drops the table entries and field hints that no pass in flight uses, so a
  // pass holds only the texts it exported.
  class Pass {
  public:
    explicit Pass(TextMetrics &metrics) noexcept;
    ~Pass();
    Pass(const Pass &) = delete;
    Pass &operator=(const Pass &) = delete;

  private:
    TextMetrics *metrics_;
  };

  // Advances of the leading glyphs of one expanded line, one per entry of
  // `ends`: the width of the line's first `ends[i]` bytes. `index` addresses
  // the line within the field whose previous measurement seeds the incremental
  // reuse. `field` is an identity token and is never dereferenced.
  [[nodiscard]] const std::vector<double> &line(const Fl_Input_ *field, int font, int size,
                                                std::size_t index, std::string_view expanded,
                                                std::span<const std::size_t> ends);

  // fl_width calls made since the pass began, and the number of cached lines.
  [[nodiscard]] std::size_t measured() const noexcept;
  [[nodiscard]] std::size_t cached_lines() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor::a11y

#endif
