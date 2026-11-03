-- The weapon on the screen (the psprites), for the 3D renderer: each layer's picture and its place on Doom's
-- 320x200 screen (x, y of its top-left corner there), drawn by play.gr scaled to the window. SQLDoom's own weapon
-- state and frames (renderer.sql's weapon_runtime and psprite_layers, kept as they are there).
--
-- Reads @map_id, @player_thing_id. One row a layer (the weapon, then its muzzle flash), in drawing order.
WITH render_context AS (
    SELECT @map_id AS map_id, @player_thing_id AS player_thing_id
), weapon_runtime AS (
    -- 'up'/'down' show the ready sprite while sx/sy slide.
    SELECT rc.map_id, rc.player_thing_id,
           COALESCE(w.sx, 1) AS sx, COALESCE(w.sy, 32) AS sy,
           -- The light amplification visor forces maximum brightness for
           -- its duration
           CASE WHEN COALESCE(ps2.light_amp_tics, 0) > 0 THEN 16
                ELSE COALESCE(w.flash_seq_index, -1) + 1 END AS extra_light,
           -- Invulnerability points Doom's fixedcolormap at COLORMAP row 32,
           -- the inverted one
           COALESCE(ps2.invuln_tics, 0) > 0 AS invuln,
           wd.sprite, wd.flash_sprite,
           CASE WHEN COALESCE(w.state, 'ready') IN ('up', 'down')
                THEN rf.frame ELSE mf.frame END AS frame,
           CASE WHEN COALESCE(w.state, 'ready') IN ('up', 'down')
                THEN rf.fullbright ELSE mf.fullbright END AS fullbright,
           ff.frame AS flash_frame, ff.fullbright AS flash_fullbright
    FROM render_context rc
    LEFT JOIN player_weapons w ON w.map_id = rc.map_id
      AND w.player_thing_id = rc.player_thing_id
    LEFT JOIN player_state ps2 ON ps2.map_id = rc.map_id
      AND ps2.player_thing_id = rc.player_thing_id
    JOIN weapon_defs wd ON wd.weapon_id = COALESCE(w.current_weapon, 2)
    LEFT JOIN weapon_frames rf ON rf.weapon_id = wd.weapon_id
      AND rf.state = 'ready' AND rf.seq_index = 0
    LEFT JOIN weapon_frames mf ON mf.weapon_id = wd.weapon_id
      AND mf.state = COALESCE(w.state, 'ready') AND mf.seq_index = COALESCE(w.seq_index, 0)
    LEFT JOIN weapon_frames ff ON ff.weapon_id = wd.weapon_id
      AND ff.state = 'flash' AND ff.seq_index = w.flash_seq_index
),
psprite_layers AS (
  SELECT 1 AS layer, wr.sprite, wr.frame AS frame,
         wr.sx AS sx, wr.sy AS sy,
         COALESCE(wr.fullbright, FALSE) AS fullbright
  FROM weapon_runtime wr
  UNION ALL
  SELECT 2, wr.flash_sprite, wr.flash_frame AS frame,
         wr.sx, wr.sy, COALESCE(wr.flash_fullbright, TRUE)
  FROM weapon_runtime wr
  WHERE wr.flash_sprite IS NOT NULL AND wr.flash_frame IS NOT NULL
)
SELECT p.layer, sl.lump_id, sf.width, sf.height, p.sx - sf.left_offset AS x, p.sy - sf.top_offset AS y,
       COALESCE(p.fullbright, FALSE) AS fullbright,
       (SELECT s.light_level FROM player_state ps JOIN sectors s ON s.map_id = ps.map_id AND s.id = ps.sector_id
        WHERE ps.map_id = @map_id AND ps.player_thing_id = @player_thing_id) AS light
FROM psprite_layers p
JOIN sprite_frames sf ON sf.sprite = p.sprite AND sf.frame = p.frame AND sf.rotation = 0
JOIN sprite_lumps sl ON sl.lump_name = sf.lump_name
ORDER BY p.layer
