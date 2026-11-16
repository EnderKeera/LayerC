# LayerE design system: visual reference

Exported from the LayerE design system on 2026-11-16 (v2.1 draft). [`../DesignSystem.md`](../DesignSystem.md) is the source of truth; this folder shows it. When the two disagree, the document wins.

- [`index.html`](index.html) — every control and screen, with a Copper / Teal / Plum / Slate switch. Open it in a browser; it works offline.
- [`tokens.json`](tokens.json) — every color, size, spacing and type token with its use. [`tokens.css`](tokens.css) is generated from it for the previews.
- `components/<Name>/` — `preview.html` and, for controls, `README.md` with the rules.
- `assets/` — the app icons (Logos), the four status glyphs (Glyphs) and the 96 DPI screenshots the Screens previews are measured from.

Limits:

- Previews are static HTML, not the GDI or FLTK painters; text renders in the viewer's own Segoe UI, or a fallback sans where it is missing.
- The System preset follows the OS or FLTK, so it has no tokens.
- The Linux main window is drawn from documented coordinates; there are no Linux screenshots yet.

Change these files only together with the matching `DesignSystem.md` change, in the same commit: export again where the design tool is available, otherwise edit the tokens, the README rules and the previews to match the document.
