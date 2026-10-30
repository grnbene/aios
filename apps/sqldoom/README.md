# SQLDoom on ASQL

A port of [SQLDoom](https://github.com/cedardb/sqldoom) (CedarDB; GPL-2.0, see `LICENSE`, which covers this directory)
to ASQL: Doom's renderer and game state in SQL, MySQL's dialect, run by aios's own engine. SQLDoom's Python parts are
grain here. Ported from upstream commit `95753a20e403d0658e624f623f972eb1f708deed`.

## Loading a WAD

```bash
aios --root R grain run --steps 0 apps/sqldoom/load.gr doom1.wad E1M1
```

`load.gr` (SQLDoom's `wad_loader.py` and `wad_sql.py`) makes database `doom`, applies `sql/schema.sql`, the import
procedures (`sql/loader_procs.sql`) and the static catalogs (`sql/game_data.sql`, `sql/menu_data.sql`), decodes the
WAD's palettes, COLORMAP, wall textures, flats, status-bar patches and sprites, loads each map asked for (all of them
when none is named), and builds the derived tables (`sql/derived.sql`). E1M1 from the shareware `doom1.wad` loads in
about 90 s.

## What differs from SQLDoom

- **Dialect.** Enum types are inline `ENUM(...)` columns, `BYTEA` is `LONGBLOB`, `SERIAL` is `AUTO_INCREMENT`,
  `ON CONFLICT` is `ON DUPLICATE KEY UPDATE` / `INSERT IGNORE`, `E'..' || E'..'` is one literal.
- **Materialized views** (`sector_adjacency`, `linedef_geom`, `node_path_steps`, `ui_hud_pixels`, `ui_static_pixels`)
  are tables `derived.sql` rebuilds after an import. `node_path_steps` walks up from each subsector instead of
  collecting PostgreSQL arrays on the way down.
- **Texels as rows.** `walltex_texels`, `flat_texels` and `sprite_texels` hold the texels the BYTEA columns hold, one
  row each, which is what the renderer joins on.

## Not ported yet

- Sound effects and music (`sound_assets`, `music_assets`, `map_music` stay empty).
- The renderer (`sql/renderer.sql`, in progress), the game tic (SQLDoom's CedarScript `sql/runtime/functions`), and
  the client.
