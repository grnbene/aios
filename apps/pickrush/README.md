# Pick Rush

A 3D forklift game whose whole state, every game tic and every frame's geometry are ASQL. Drive a forklift round a
warehouse, pick pallets out of rack slots for the open orders and set them down on the shipping dock; put away what
turns up at receiving. Three other forklifts drive the aisles.

It exists to drive ASQL with a workload sqldoom does not: ERP-shaped tables (slots, SKUs, orders, order lines,
inbound stock) changed every 1/30 s by stored functions, and a per-frame query that is all floating-point
arithmetic over joins - the vertex half of a renderer.

## Running it

```bash
aios --root R grain run --steps 0 apps/pickrush/load.gr          # build the warehouse (again, to start over)
aios --root R grain serve apps/pickrush/play.gr                  # play, in a browser
aios --root R grain run --steps 0 apps/pickrush/autoplay.gr 6    # a script plays six tasks through the same tic
```

Keys: W/S or Up/Down drive, A/D or Left/Right steer, R and F raise and lower the forks, Space picks up or sets down,
C switches between the chase camera and the cab. A slot is picked from or put into with the forklift square to the
rack (facing north) and the forks at the slot's height; the HUD says what is in front of the forks and how far off
their height is.

`PICKRUSH_FRAMES=n` runs n tics and frames with no display and saves the last (`PICKRUSH_SHOT`), with the keys in
`PICKRUSH_KEYS` held; it prints each frame's times. `autoplay.gr` fails if a pick, put-away or shipment it makes
does not land; `AUTOPLAY_SHOT=dir` saves a frame of each step.

## How it is split

| Part | Where | What |
|---|---|---|
| World | `sql/schema.sql`, `sql/build_world.sql` | Everything is a box: a part is a box on an entity (rack row, pallet, forklift), at an offset in the entity's frame; slots, SKUs, orders, the aisle network |
| Game tic | `sql/tic.sql` | `pr_tic(throttle, steer, lift, action)`: driving and collision (`pr_blocked`, against the solid boxes and other trucks), the forks, picking and setting down (`pr_action`), the other trucks (`pr_bots`), orders, lateness and inbound stock (`pr_business`) |
| Vertex stage | `sql/frame.sql` | One query: each part placed in the world, culled against the view, broken into the faces that look at the camera, lit and fogged; camera-space triangles out |
| Fill | `r3d_*` builtins (`src/userland/grain_std_raster3d.cpp`) | Near-plane clipping, projection, a depth-buffered fill with box edges outlined |
| Client | `play.gr` | Keys, the 30 Hz tic clock, the chase and cab cameras, the HUD |

## What it found in ASQL

- **0x literals into numeric columns** stored their bytes: `INSERT INTO parts (color) VALUES (0x9a7444)` kept three
  bytes that every read returned as text, through VALUES, `UPDATE .. SET`, the literal `ON DUPLICATE KEY UPDATE`
  path and `INSERT .. SELECT 0x..`; an odd digit count padded at the wrong end (`0xABC` read as `0xAB0C`).
  MariaDB stores the number (`tools/asql-hex-literal-check.cpp`).
- **A stored function run twice, its error lost**: a routine's fast expression path ran a function as it read it;
  when it then gave up, or the function failed, the expression ran again as a SELECT - every write twice, and a
  function that failed once could succeed the second time (its first writes now there), so `pr_tic` created an order,
  failed on its lines and went on as if nothing had happened (`tools/asql-routine-once-check.cpp`).
- **A frame was 42 ms, now 5**: a `SET @cy = 3.6` camera is a DECIMAL, and every expression that met it went row by
  row; so did `ROUND` of a DOUBLE and a bit operator's result times a DOUBLE; and `sql()` got its 33,000 numbers
  printed to text and parsed back. DECIMAL columns of at most 15 significant digits now compile beside doubles,
  ROUND of a DOUBLE and unsigned-times-double are vectorized, and `sql()` reads a SELECT's typed columns
  (`execute_local_query_columns`). Each checked by running the frame queries with the vectorizer and typed batches
  off (`AIOS_SQL_VPROG=0`, `AIOS_SQL_COLUMNAR=0`): the same rows in the same order.

Left as found: a camera variable with more than 15 significant digits (`SET @cx = 1.5707963267948966`) is an exact
DECIMAL finer than a double, so expressions over it are still worked out row by row; `play.gr` passes the camera as
DOUBLE literals (`31.5e0`), as sqldoom's client does.
