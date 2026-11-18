# Bin Capacity

How full every bin is, and where a tire size still fits - from bin sizes measured with a phone and the stock the
ERP already records. ASQL keeps the answer live: when a pick, a putaway or a bin move changes `locstock`, the bins it
touched are worked out again before the write returns.

---

## How it works

1. **A bin's size, from a photo.** Print the marker (`/marker.png`: a black square with a white square half its
   side in the middle, 150 mm across at 100%; measure the print and set `MARKER_MM` to what it is). Tape it flat on
   the bin's front - the beam under the opening is a good place - and photograph the bin with the phone. The page
   finds the marker (`image_find_marker`), you drag four handles onto the opening's corners, and the opening is
   measured in the marker's plane (`homography`): width and height, each the mean of its two edges. The depth is
   typed (default `BIN_DEPTH_MM`). A size can also be entered by hand (`POST /api/dims`). Sizes go in `bin_dims`,
   photos in `bin_photos`, with who and when.
2. **What a bin holds of a size.** A tire is a cylinder: its width and diameter come from its size in `tireinfo`
   (`225/45R17` is 225 mm wide and 634 mm across). `TIRE_FIT(w, d, h, size)`, an ASQL function, is how many fit
   the bin: the best of lying in stacks (circles packed on the floor, straight or offset rows) and standing either
   way round. The same code is grain's `tire_fit` (`include/aios/userland/tire_fit.hpp`).
3. **How full it is.** The `bin_fill` Thing (lib/bins.gr) sums, over what `locstock` has in each bin of
   `BIN_LOCCODE`, each item's quantity over what the bin holds of it alone - 1.0 is full whatever the mix. Mixed
   sizes pack a little worse than that: treat 0.9 of a mix as nearly full. Stock whose item has no tire size is
   counted as `unplaced`, not guessed at.
4. **Where a size fits.** For each measured bin, its free share times what it holds of that size.

## From the photo alone

- **Depth from a tap.** The blue handle goes on the back corner of the bin's floor. The camera and the opening are
  found together (`camera_pose_rect`: the marker gives the scale, the opening's right angles the lens and the tilt;
  the lens from the photo's EXIF when the phone wrote one, read in the browser before it shrinks the photo). The
  floor is the plane through the opening's bottom edge square to the front; the tap's ray meets it at the depth.
  On the rendered check (tests/grain/test_bin_photo.gr) a 1200 x 1000 x 900 mm bin measures 1200.7 x 1000.6 x 899.3
  with no lens given, the lens itself 1383 px against 1386.
- **What is really in it.** Tick "the bin is empty now" when measuring an empty bin: that photo and its camera are
  kept. Any later photo of the bin's front with the marker in view (`POST /api/check`, "Check a bin" on the page)
  needs no tapping: the opening is found through the marker, the empty bin is re-made as this photo's camera would
  see it (`box_face_views`: its walls, floor and back are planes, so the empty photo can be re-projected from
  anywhere in front), and the share of the opening that differs is what is in it (`image_difference`, lighting
  evened). That is set against the ERP's fill: `erp_has_more` (the ERP has stock there, the bin looks empty),
  `photo_has_more` (something is there the ERP does not have), `agrees`, or `unsure` (too little could be
  compared). Checks are kept in `bin_checks`. The photo sees the front, the ERP counts the volume: only a clear
  contradiction is flagged.

## Measuring well

- The marker must be flat and in the plane of the opening; if the opening's opposite edges disagree by more than
  5% the page says so (`skew`), and the photo should be taken again.
- A larger marker measures large bins better: a 150 mm marker is about 0.5% on a 1.2 m opening at a moderate angle.
  Print it bigger and set `MARKER_MM` for wide racking.
- The whole marker must be in the photo with white round it.

## Settings

| Setting | Default | What |
|---|---|---|
| `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_NAME`, secret `DB_PASSWORD` | `127.0.0.1`, `3307`, `root`, `versioneight` | the ASQL holding `locstock` and `tireinfo` |
| `BIN_LOCCODE` | `01` | the location whose bins these are |
| `MARKER_MM` | `150` | the printed marker's black square, measured |
| `BIN_DEPTH_MM` | `1000` | a bin's depth unless typed |

`locstock` is read as `(loccode, stockid, bin, quantity)` and `tireinfo` as `(stockid, size)`.

## Running it

```bash
aios exec "app deploy apps/bin_capacity"            # deployed.gr makes the tables and the bin_fill Thing
aios exec "app set bin_capacity BIN_LOCCODE=01"
```

The page is on port 8575 (`listen` is local: put it behind the site's nginx, as the other warehouse apps are, for
phones on the floor).

## Routes

| Route | What |
|---|---|
| `GET /` | the page |
| `GET /marker.png` | the marker to print |
| `GET /api/board` | every measured bin, fullest first |
| `GET /api/bin?bin=B` | what is in a bin, how many of each it holds, each item's share |
| `GET /api/room?size=S` | where tires of size `S` still fit |
| `POST /api/photo?bin=B` | a photo of the bin's front: the marker found, the photo kept |
| `POST /api/measure` | `{photo_id, opening, back, focal, depth_mm, empty}`: the size measured (depth from the `back` tap, else `depth_mm`) and saved; `empty` keeps the photo to check against |
| `POST /api/check?bin=B&focal=F` | a photo of the bin's front: `{changed, compared, erp_fill, verdict}`, kept in `bin_checks` |
| `POST /api/dims` | `{bin, width_mm, depth_mm, height_mm}`: a size entered by hand |

## Checks

`tests/grain/test_bin_photo.gr` renders a bin in 3D from known cameras and checks the size and depth from one
photo and a tap, and empty / stocked against the empty photo from two other viewpoints.
`tests/grain/test_bins.gr` (run with `aios grain run --steps 0`) measures a bin's front drawn through a known
perspective, saves it, puts stock in, and checks the Thing's fill against `tire_fit` as stock and sizes change;
`tests/grain/test_measure.gr` checks the marker, homographies and tire sizes.
