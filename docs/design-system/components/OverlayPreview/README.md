# OverlayPreview

How the editor shows the overlay: in Add overlay item and Edit overlay item, at actual size, and in the main window's preview corner. It draws like `overlay_render.cpp`, not in the editor's tokens, so what you see is what the game shows, vivid colors included.

- **One painter.** The preview uses the overlay's own geometry (`panel_geometry`, `bar_geometry`, `item_pip_layout`) and metrics: Segoe UI Semibold 14 pt labels on one line, centered and cut with an ellipsis; bar corners `corner_radius` 8 and fills `fill_radius` 6 (RoundRect ellipse sizes); cards 78 × 72; pips 8 px apart by 3 px; a grouped bar as a track with a 2 px inset fill; the selected panel outlined 2 px #E0A15A, the renderer's edit-mode pen. Never a second drawing of the overlay.
- **Item dialog.** Fields in one column at the left: Kind, Source, Label (Custom check box and its edit), the kind's own fields (Timer source, Counter max and Display, Flash (Hz), Interval), then Color. The preview sits at the right in a 300 × 150 well at 1:1, with the panel named under it ("Panel “Buffs” · card · row").
- **Show** picks the state: Ready or Cooling (cooldown); Running or Stopped (timer); a value (counter); Lit, Lit flashing or Off (indicator); Unavailable for every kind. **Backdrop** picks Dark or Bright, to check the overlay over a light game.
- **Motion.** A cooldown plays its duration and an indicator flashes at min(flashHz, 3) Hz. Both stop when Windows' Show animations is off (`SPI_GETCLIENTAREAANIMATION`); Linux starts still. Show still picks any state by hand.
- **Readability lines** under the preview, one StatusText line each, warning but never blocking (Overlay): text against fill and against background at 4.5:1, marks against background at 3:1, and a label cut to fit ("“Shield up” is cut to fit the 78 px card.").
- **Color:** a color button showing the item's hex, starting as the panel's fill (`OverlayItem::color`); Choose color opens with the overlay floors.
- **Buttons:** Add item when adding, Save item when editing, as the primary; then Cancel.
- **Preview corner:** the same painter scaled into the target frame, with the selected item at actual size under it.
- **Screen readers:** the preview is one image whose description names the item, its state and any readability line.

The consumer supplies the profile, the panel, the item and the state to show. The overlay colors in the preview are sample profiles.
