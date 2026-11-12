# OverlayBar

The in-game bar in a profile's own colors, checked against the Overlay rules: text 4.5:1 on fill and background, marks 3:1 on the background.

- **Example profile** (from the repository README): fill #50BAC8, background #202A31, border #80D8E8, text #F1F5F7. Its text is 2.08:1 on the fill; #141414 is 8.07:1.
- **iwcs:** text #F1F5F7 on fill #DDBC17 is 1.70:1; #141414 is 9.90:1.
- **Contrast line:** under the Fill, Background, Border and Text buttons, while a pair is below its minimum, one `warning` line names the pairs and the target: "! Low contrast. Text is 1.7:1 on Fill and 1.9:1 on Background; aim for 4.5:1." A Border below 3:1 on Background adds "Border is 1.4:1 on Background; aim for 3:1." The line goes once every pair passes; it never changes the profile or blocks a save.
- **Choose color** opened from those buttons shows the lowest ratio the color makes against its partners: Text against Fill and Background, Fill and Background against Text, Border against Background. "! Contrast with Fill: 1.7:1; aim for 4.5:1."
- **Flashing:** an indicator flashes at 3 Hz at most (WCAG 2.3.1). Profiles keep `flashHz` 0 or 0.5–4 unchanged; every renderer flashes at min(`flashHz`, 3) through one core helper, and the editor's field accepts 0 or 0.5–3, warning "! Flashes at 3 Hz; faster flashing is capped." on a loaded value above 3.

The consumer is the overlay renderer; the profile supplies every color and size. Item shapes in the preview are illustrative.

The preview is a static HTML rendition of these rules.
