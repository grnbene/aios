# Photos into assets and measurements: where it stands

State on `main` at a138b75 (branch `claude/visual-sprite-carver-ypkr41`).

## Done
- Doom from photos: `apps/sqldoom/carve.gr` (one photo, or a folder shot round the printed mat), `capture.gr`
  (phone page). Real lion photo verified as the imp, both views; `carve_check.gr` passes.
- Bins: `apps/bin_capacity` - marker measuring, depth from one tap, stock check against the empty photo
  (light-invariant), live camera guidance (`/api/frame`), ERP fill via the `bin_fill` Thing.
- Measure anything in an ordinary photo (no marker): `lib/measure.gr`, `/api/scene*`, page card; box -> .glb.
- Tests: `tests/grain/test_{carve,measure,cameras,reconstruct,bins,bin_photo,bin_dark,photo_measure}.gr`
  (run with `aios --root R grain run --steps 0 FILE`).

## Next, in order
1. Guidance: use `image_find_marker`'s `edge_px` (blur in pixels) in `bins_frame` - "Hold still" when over
   about 2 px at 640 wide; today steadiness is only relative to recent frames.
2. Doom wall textures from a real photo: rectify (`image_rectify`), make tileable, `image_quantize` to the
   palette, write `walltex_meta` / `walltex_texels` (see `apps/sqldoom/load.gr` for the layout).
3. A gun from a real photo: as the pistol's model (`apps/sqldoom/lib/arms.gr`) and its PISG sprites.
4. Warehouse map: a coded marker per bin (`marker_image`, `markers_find`), photos seeing two or more link them,
   solve every bin's position; table plus a map view.
5. A true bin: needs a real photo of a bin with `/marker.png` taped on it, checked against a tape measure.

## Known limits
- The live camera needs https on a phone.
- The standalone `grain` build (grain/CMakeLists.txt) was already broken on main: raster3d needs ASQL.
- Measuring with no marker: the lens comes from the lines only when they converge strongly; the scale comes
  from one tapped reference, so its accuracy bounds the result.
