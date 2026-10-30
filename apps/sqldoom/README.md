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

## Rendering a frame

```bash
aios --root R grain run --steps 0 apps/sqldoom/render.gr out.png [MAP [x y z angle]]
```

`render.gr` sets the renderer's parameters (`@map_id`, `@player_thing_id`, `@skill`, `@x`, `@y`, `@z`, `@angle`),
runs `sql/renderer.sql` - SQLDoom's whole renderer, one statement - and saves the 320x200 frame it returns. With no
pose the camera is at player 1's start:

![E1M1 from player 1's start, rendered by ASQL](e1m1-start.png)

It takes about 15 s a frame (SQLDoom on CedarDB: about 13 ms). Most of that is ASQL evaluating the per-pixel
CTEs (the floor and ceiling ones run over some 56,000 rows each, a second or two apiece).

## What differs from SQLDoom

- **Dialect.** Enum types are inline `ENUM(...)` columns, `BYTEA` is `LONGBLOB`, `SERIAL` is `AUTO_INCREMENT`,
  `ON CONFLICT` is `ON DUPLICATE KEY UPDATE` / `INSERT IGNORE`, `E'..' || E'..'` is one literal.
- **Materialized views** (`sector_adjacency`, `linedef_geom`, `node_path_steps`, `ui_hud_pixels`, `ui_static_pixels`)
  are tables `derived.sql` rebuilds after an import. `node_path_steps` walks up from each subsector instead of
  collecting PostgreSQL arrays on the way down.
- **Texels as rows.** `walltex_texels`, `flat_texels` and `sprite_texels` hold the texels the BYTEA columns hold, one
  row each, which is what the renderer joins on, by one packed integer key (`id * 2^20 + v * 2^10 + u`): ASQL
  indexes a join column by column, so a join on (name, u, v) checked thousands of candidates per pixel.
- **The renderer** (`sql/renderer.sql`, whose header lists every change): MySQL casts and `DIV`; texel joins instead
  of `GET_BYTE`; the top-1 `LEFT JOIN LATERAL`s as window functions; `BOOL_AND`, `ARG_MAX`, `string_agg`, `||`,
  `IS DISTINCT FROM` and `FULL JOIN` in their MySQL forms; parameters as session variables. The light functions
  (`sql/render_light.sql`) are stored functions.

## Not ported yet

- Sound effects and music (`sound_assets`, `music_assets`, `map_music` stay empty).
- The game tic (SQLDoom's CedarScript `sql/runtime/functions`) and the client. Until the tic spawns the player,
  `render.gr` puts a `player_state` row with the schema's defaults there (hence the 0 ammo above).
