# Web engine vs Chrome: what is left

How the engine's pages differ from desktop Chrome's, and what was done about it. Checked on 2026-10-23 by
rendering the same pages in `aios-web-shot` (the browser pane, painted offscreen) and in headless Chrome side by
side, and held by the layout goldens in `scripts/web-corpus` (`scripts/drive-web.sh`, 159 checks), which carry a
page for each item below.

## Closed

| Gap | What the engine does now | Where |
|---|---|---|
| Checkboxes and radios drawn as `[x]` / `(*)` | Chrome's 13px controls: a 2px-rounded box with a tick, a ring with a dot, in Chrome's colours for a light or a dark page, `accent-color` honoured, greyed when disabled, nothing drawn under `appearance: none`. A click (or a click on the `<label>`) checks it, unchecks the other radios of its name, and fires `click`, `input` and `change`; `preventDefault()` puts it back. | `web_layout.cpp` `control_mark`, `web_pane.cpp` `activate_checkable` |
| Buttons, fields, dropdowns | Chrome's UA values: field padding 1px 2px with a 2px radius, buttons and dropdowns 3px, labels centred in buttons, a drawn chevron on a closed `<select>`, disabled controls muted, a hover shade on buttons. A control's CSS margins are kept (a row of buttons is spaced). A control sits on the line by its label's baseline. | `web_css.cpp` UA sheet, `web_layout.cpp` Field items |
| Gauges | `<progress>`/`<meter>` as an 8px rounded track with an edge, Chrome's fill colours, `accent-color`. | `web_layout.cpp` |
| Line height | `line-height` is honoured below the face's own height (a 1.1 heading is set tight); the leading is split above and below the text (half-leading), a line is as tall as its highest ascent plus its deepest descent, and `line-height: normal` resets instead of inheriting. An inline-block sits on its last line's baseline. | `web_layout.cpp` `text_box`, line builder, `last_baseline` |
| Kerning | DejaVu's `kern` pairs (Latin and punctuation, all eight sans and serif faces) are applied when text is measured and drawn. Generated from the same font files as the atlas by `metal/tools/mkkern.py`. | `font_kerning.cpp`, `web_type.cpp` |
| Focus ring | 2px, following the control's own radius, Chrome's `#005FCC` on a light page. | `web_pane.cpp` |
| Scrollbars | The page's bar follows its ground (light track and thumb on a light page). An inner `overflow: auto/scroll` box with more in it than fits shows a thumb inside its edge. A scroll pane's own border and background are no longer clipped away by its own overflow. | `web_pane.cpp`, `web_layout.cpp` |
| Underlines | Full ink, thickness and offset scaled with the font size (strike-through likewise). | `web_pane.cpp` |
| SVG | `<use>` (of an element, or a `<symbol>` with its viewBox), nested `<svg>`, `transform` (translate, scale, rotate, skew, matrix), linear and radial gradients (units, `gradientTransform`, `href` chains, `stop-opacity`), `fill-opacity` / `stroke-opacity` / `opacity`, `style="fill: ..."`, every CSS colour form, `fill-rule="evenodd"`, `preserveAspectRatio` (align, meet, slice, none). | `web_svg.cpp` |
| CSS transforms | `scale`, `rotate`, `skew`, `matrix`, `matrix3d` (its 2D part), `rotateX/Y` (flattened as Chrome does without perspective), the standalone `rotate` and `scale` properties, `transform-origin`, nesting. The box keeps its place in the flow; its fragments are painted offscreen and composited through the matrix, and links inside it are hit where they are drawn. | `web_css.cpp`, `web_layout.cpp` `add_transform`, `web_pane.cpp` `render_group` |
| Repaint after a change | A relayout that kept every fragment's count and size (a ticked checkbox, a swapped class) left the old picture on screen; the frame cache now follows a layout generation. Inline pictures are no longer appended again on every relayout. Text is one fragment per element, so a click on a `<label>` or a `<span>` with a handler reaches that element. | `web_pane.cpp`, `web_layout.cpp` |

## Left

1. **The typeface.** The atlas is DejaVu, whose advances are about 13% wider than the faces Chrome uses by default
   (Arial, Helvetica, Liberation Sans). Advances are narrowed by 6% and spaces set at 0.26em as a fit, but text still
   sets wider than Chrome's: a 40px bold heading 420px wide takes three lines where Chrome takes two. The real fix is
   to cut a metric-compatible sans (Liberation Sans or Arimo, both freely licensed) into the atlas with
   `metal/tools/mkfont.py` and kern it with `mkkern.py`.
2. **Shaping beyond pairs.** Kerning comes from the `kern` table only: no GPOS contextual positioning, no ligatures
   (`fi`, `fl`), no complex-script shaping. Monospace text is not kerned (correctly).
3. **Subpixel (LCD) antialiasing: not done, on purpose.** Text is grayscale-antialiased, as Chrome on macOS draws it.
   LCD antialiasing depends on the panel's subpixel order, and this engine's output is scaled (zoom), rotated (CSS
   transforms), streamed as pictures and shown on displays it cannot ask; on any of those it turns into colour fringes.
4. **Transforms.**
   - Transformed content is resampled after it is painted, not re-rasterised in place, so rotated text is slightly
     softer than Chrome's.
   - Clicks on elements inside a rotated or scaled box are tested against their untransformed boxes. Links are mapped.
   - No perspective or 3D.
   - Transitions and `@keyframes` animations are not implemented at all, for transforms or anything else.
5. **SVG** does not draw clip paths, masks, patterns, filters, markers, `<text>`, a radial gradient's focal point, or
   the `reflect` / `repeat` spread methods.
6. **States.** `:hover` applies on the frame after the pointer moves. (`:active` matches while the button is down
   since 2026-10-28: the pane marks the pressed element with `data-aios-active` on press and clears it on release,
   as `:hover` is done; `scripts/drive-active.sh` holds a button down and reads the fragments.)
7. **Missing images** show their alt text in a box; Chrome shows a broken-image icon beside the alt text.
8. **A page that sets no background** is drawn on the engine's dark ground (the machine's own look), where Chrome uses
   white. A deliberate difference, not a gap.
