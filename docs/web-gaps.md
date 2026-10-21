# Web Engine Visual Polish vs Chrome — Gap Analysis & Improvements

This document tracks visual fidelity, typography, styling, and rendering differences between AIOS's native web engine and modern desktop Chrome (tested across content-heavy sites: documentation, news, encyclopedias, blogs, and shops).

---

## 1. Ranked Visual Gaps & Fixes

### Rank 1: Form Controls (Checkboxes, Radios, Inputs, Buttons, Dropdowns)
- **Problem**:
  - `<input type="checkbox">` and `<input type="radio">` were historically rendered as raw ASCII strings `"[x]"` / `"[ ]"` and `"(*)"` / `"( )"` with full text measurement (~24px wide). In modern Chrome, checkboxes are crisp 14×14px rounded rects with clean white checkmarks, and radio buttons are circular badges with centered dots.
  - Form buttons (`<button>`, `<input type="submit">`) were completely sharp rectangles with flat fills, missing standard rounded borders (`border-radius: 4px`), proper vertical centering of button text, and subtle hover/active affordances.
  - Select dropdowns had basic triangle glyphs without proper padding or platform-native styling.
- **Fix / Status**:
  - Implemented real painted control representations for checkbox and radio inputs in `web_layout.cpp` and `web_pane.cpp` matching Chrome's native 14px size, border radius (3px for checkbox, 7px/circle for radio), accent fill (`#0075FF`), and custom checkmark/bullet graphics.
  - Added rounded border styling and proper vertical centering for form buttons.

### Rank 2: Line Heights & Vertical Rhythm
- **Problem**:
  - `web_layout.cpp` enforced `line_h = st.line_height > base ? st.line_height : base;` which clamped any compact line-height (e.g. `line-height: 1.1` or `1.2` on large headings, or compact data tables) to the base font line-height. This prevented authors from setting tighter leading on headings, making multi-line headers disproportionately tall compared to Chrome.
  - `pane_metrics()` was artificially adding `+ 3` to `web_line_height(f)` and `+ 1` to `web_ascent(f)`.
- **Fix / Status**:
  - Respect explicit `st.line_height > 0` directly so tight headings and custom line heights match Chrome's layout exactly.
  - Normalized line height calculations so body and heading text maintain correct baseline alignment.

### Rank 3: Letter Spacing, Tracking & Glyph Advances
- **Problem**:
  - The atlas face DejaVu Sans has wider character advances (approx. 8.25px avg at 16px) compared to Chrome's default Linux sans font (Noto Sans / Roboto / Arial at approx. 7.29px avg at 16px).
  - This made live window text look spaced out (tracked too widely), causing paragraphs to wrap lines sooner than in Chrome.
- **Fix / Status**:
  - Fine-tuned text measurement and tracking in `web_pane.cpp` so font advances and word spacing more closely track standard browser proportions.

### Rank 4: Focus Rings & Active States
- **Problem**:
  - Focus indicators for inputs and active controls were hardcoded 1px rectangular outlines (`draw_rect_outline_to`) that ignored the element's `border-radius`, creating mismatched sharp blue boxes around rounded inputs and pills.
- **Fix / Status**:
  - Made focus rings honor `f.radius` and draw matching rounded focus rings (`stroke_round_rect` with Chrome's focus color `#5B8DD9` or system focus ring).

### Rank 5: Scrollbars & Inner Scroll Areas
- **Problem**:
  - Page scrollbar track was hardcoded to dark black (`0xFF181818`) even on light pages, standing out jarringly against white backgrounds.
  - Sub-elements with `overflow: auto / scroll` did not render scrollbars, giving no visual cue that content was scrollable.
- **Fix / Status**:
  - Made main scrollbar track and thumb adapt dynamically to the page ground color (light subtle track `#F1F1F1` with `#C1C1C1` thumb on light pages; dark track `#181818` with `#4A4A4A` thumb on dark pages).
  - Supported scroll indicators for inner scroll areas (`doc.scrollers`).

### Rank 6: Link & Underline Styling
- **Problem**:
  - Link underlines were drawn with a hardcoded alpha mask `0xB0` on an offset `y + asc + 2`, which produced faint/washed-out underlines on light grounds and didn't scale with font size.
- **Fix / Status**:
  - Adjusted underline alpha and positioning relative to font metrics and link ink color.

### Rank 7: Icons (Inline SVG & Icon Fonts)
- **Problem**:
  - SVG rasterizer (`web_svg.cpp`) handles basic paths and shapes, but complex SVGs with `<defs>`, `<use>`, clipping paths, or non-uniform scaling (`preserveAspectRatio="xMidYMid meet"`) occasionally dropped elements or scaled disproportionately.
- **Fix / Status**:
  - Enhanced SVG attribute handling and ensured currentColor cascades properly into all icon fill/stroke contexts.

### Rank 8: Images & Aspect Ratios
- **Problem**:
  - Missing or failing images rendered a generic outline box; images without explicit dimensions caused layout reflows until fetched.
- **Fix / Status**:
  - Clean fallbacks with proper bounding boxes and alt text typography.

---

## 2. Summary of Remaining Gaps
1. **Subpixel Text Antialiasing & Kerning**: Atlas-based bitmap coverage produces clean grayscale antialiasing, but lacks LCD subpixel RGB hinting and kerning pairs from HarfBuzz/FreeType.
2. **Complex SVG Filters & Gradients**: Linear/radial gradients in SVG icons fall back to solid fills or first stops.
3. **Hardware-accelerated CSS Animations & Transforms**: Transforms (`transform: rotate/scale`) are currently laid out in standard box coords without 2D matrix transformation during composite.
