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
| `POST /api/measure` | `{photo_id, opening, depth_mm}`: the size measured and saved |
| `POST /api/dims` | `{bin, width_mm, depth_mm, height_mm}`: a size entered by hand |

## Checks

`tests/grain/test_bins.gr` (run with `aios grain run --steps 0`) measures a bin's front drawn through a known
perspective, saves it, puts stock in, and checks the Thing's fill against `tire_fit` as stock and sizes change;
`tests/grain/test_measure.gr` checks the marker, homographies and tire sizes.
