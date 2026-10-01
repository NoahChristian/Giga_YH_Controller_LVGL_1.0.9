# How to mock up this dashboard's screens accurately (HTML/CSS)

## The method

1. **Composite on a real capture -- never hand-redraw existing UI.**
   `python dump_screen.py COM9 <name>`, then embed the PNG as the
   mockup's background (`data:image/png;base64,...`). Everything
   untouched in the capture is pixel-perfect for free; hand-drawn CSS
   approximating LVGL's real font rendering never fully matches it
   (different rasterizer, hinting, glyph metrics), no matter how
   carefully it's measured. Paint a patch (real background color,
   sampled from the capture) over only the region that's changing, and
   draw the new proposed UI on top of that patch.

2. **Generate the HTML with a script that reads the base64 from disk --
   never retype/reproduce it yourself.** A ~20K+ character blob passed
   as a literal through your own text generation is a real corruption
   risk (silent PNG scanline desync from one altered character shows up
   as magenta/pink streaking). Use an f-string or a template with one
   clearly-defined quote style, not hand-nested escaping -- a
   `'''Montserrat'''`-style quoting bug renders fine (falls back to
   default sans-serif silently) so it won't be visually obvious; grep
   the generated file for the property and confirm the value is
   well-formed.

3. **Position everything as `%` of the container** (`left`/`top`/
   `width`/`height`, computed as `measured_px / 800 * 100` and
   `measured_px / 480 * 100` -- the logical display is 800x480, and PNG
   pixel coordinates already match `lv_obj_set_pos()` 1:1, no rotation
   math needed since the exporter un-rotates captures before writing
   them). `%` scales correctly with the container at any render size.

4. **Size overlay text/shapes in `cqw`, never `vh`/`vw`/fixed `px`.**
   `vh`/`vw` are viewport-relative, not container-relative -- unrelated
   to how large the artifact actually renders. A fixed `px` value only
   happens to look right at whichever one width it was eyeballed
   against. Put `container-type: inline-size` on the aspect-ratio
   wrapper and size in `cqw` (`% of container inline-size`), computed
   the same way as position percentages.

5. **Get the `cqw` value from a live-rendered, verified measurement --
   not from the capture's pixel height directly, and not from theory.**
   CSS `font-size` is not glyph ink height; the gap is real and
   font-specific (Montserrat's cap-height is ~0.72 of its em-square, so
   naively using measured ink height as the `cqw` numerator renders
   ~30% too small). Serve the mockup locally (`python -m http.server`,
   launched detached via `nohup ... & disown` so it survives the
   launching tool call) and drive a real browser:
   - `getComputedStyle(el).fontSize` -- confirms the `cqw` arithmetic
     resolved as intended (sanity check only, not the visual answer).
   - `canvas.getContext('2d').measureText(str).actualBoundingBoxAscent
     + actualBoundingBoxDescent` -- the *true* rendered ink height for
     specific text at a specific font, comparable directly against a
     capture's measured glyph height.
   - Solve for the `cqw` that makes measured ink height match the
     capture's, via a small iterative correction loop (adjust by the
     measured-vs-target error, repeat ~20x) -- don't hand-derive the
     font's cap-height ratio, just close the loop empirically.
   - If a screenshot tool is unavailable or hangs in the environment,
     `getComputedStyle` + `measureText` gets a fully verified numeric
     answer with no visual render needed at all.

6. **Verify layout with real bounding-box checks, not eyeballing --
   check every state, and check siblings against each other, not just
   against their container.** Two independent failure modes, both
   real bugs caught this way:
   - *Patch too small for its content*: a patch sized for one state's
     content can be too small once another state's (e.g. a two-line
     variant) content is corrected to the right size. Check
     `el.getBoundingClientRect()` is fully inside
     `patch.getBoundingClientRect()`, **for every state that changes
     content**, with real margin (`patch.bottom - el.bottom`
     meaningfully positive), not exact-fit.
   - *Siblings overlapping each other*: containment-in-patch passing for
     each element individually does not mean two sibling elements don't
     overlap each other -- check that directly too:
     `elementBelow.top - elementAbove.bottom` must be positive with real
     margin, for every state.

## Reference measurements (Home screen, `home_pixel_measure_20260716-012955.png`)

All in real device pixels (800x480). Measured via ink-band row/column
scanning (scan for "any non-background pixel in this row/column" to get
contiguous bands), not single-point color search -- same-colored
elements elsewhere in frame will contaminate a naive search (e.g.
searching for the SoC ring's red track also matches "Charging" text,
which shares `COLOR_RED`). Cross-check any automated measurement with a
2x-zoomed crop of the region before trusting it.

| Element | Position | Size |
|---|---|---|
| Weather pill | x=75-324, y=136-175 | 250x40 |
| Rate pill | x=97-302, y=192-231 | 206x40, gap 16px below weather pill |
| Connection dot | x=500-509, y=91-100 | 10x10 |
| "Connected" text row | y=83-106 | -- |
| SSID line | y=124-139 | gap 17px below Connected row |
| SoC ring (Home quadrant) | x=92-175, y=297-380 | 84x84 |
| Grid-quadrant rings (both) | y=257-320 | 64x64 (source requests 65, renders 64) |
| Left column watts | y=342-363 | gap 21px below ring |
| Left column "Battery" | y=382-409 | gap 18px below watts |
| Left column "Feeder" | y=420-443 | gap 10px below "Battery" |
| Right column watts | y=342-363 | gap 21px below ring (matches left) |
| Right column "Grid" | y=380-403 | gap 17px below watts |

Pill background measured as `rgb(16,24,24)` (`#101818`) -- not the naive
`COLOR_TEXT`-adjacent guess `#17191c`, close but not identical, likely
RGB565 quantization on the real panel. Screen background measured as
`rgb(8,12,8)` (`#080c08`). Overlay text font-size that matched the real
panel: `4.2cqw` (Montserrat, verified via the ink-height method above,
within ~1.3% of target).

## Other confirmed facts

- **LVGL arc angle 0 = the 3 o'clock/east point, sweeping clockwise** --
  matches SVG's own default stroke-dasharray start point, so an arc
  circle needs no rotation transform to match `lv_arc_set_angles(ring,
  0, angle)`. Don't add `transform="rotate(-90 ...)"`.
- **Pills are fully rounded** (`lv_obj_set_style_radius(pill,
  LV_RADIUS_CIRCLE, 0)` -- stadium/capsule shape, `border-radius: 50%`,
  not a small fixed corner radius). Pill width is `labelW + 2*padX`
  (`padX` is `makeAutoPill()`'s own explicit parameter, 16px at every
  call site so far) -- use that exact value.
- **Element sizes from source generally match reality; gaps between
  stacked text lines don't.** `makeDot(..., 10)` really is 10x10px on
  the real panel -- trust source-declared widths/diameters for shapes.
  But vertical gaps between labels are governed by real font
  ascent/descent/line-height, which the sketch's LVGL font and a
  browser's Google Fonts Montserrat don't share -- a source Y-offset
  (`lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 118)`) describes where LVGL
  was told to put the box, not the visible whitespace between two
  rendered lines. Measure the real gap from a capture instead.

A small reusable PNG reader/writer (no PIL) for scripted pixel analysis
lives inline in `dump_screen.py`; the same pattern was reused ad hoc for
measurement scripts this session -- worth factoring into a shared
`tools/measure_capture.py` if this becomes a repeated task.
