-- What stands in the world this tic, after sql/client/scene_common.sql: monsters, items, decorations, effects and
-- projectiles, one row each, for play.gr to draw - a creature as its model posed for what it is doing, anything
-- else as its sprite frame turned to the eye each frame. SQLDoom's own choice of each thing's frame (thing_view,
-- kept as renderer.sql has it).
--
-- One row a thing: thing_id (negative for effects and projectiles), type (its thing type; NULL for those), x, y,
-- z (its feet), angle (degrees), sprite and frame, fullbright, light (its sector's, 0..255), fuzz, state (what its
-- AI is doing: stand, see, missile, pain, die, xdeath, dead) and alive.
,
thing_view AS (
  SELECT
    CAST(t.id AS SIGNED) AS thing_id, t.x, t.y, t.angle,
    -- Sprite lump name never changes across a monster's own states.
    -- Only the frame letter changes. Vanilla Doom always keeps one 4-letter
    -- sprite per mobj type. It only ever changes for the terminal death
    -- sprite (e.g. exploding barrels: BAR1 -> BEXP).
    CASE WHEN COALESCE(d.explodes, FALSE) AND ai.state='die' THEN d.death_sprite
         WHEN h.alive = FALSE AND (ai.state IS NULL OR ai.state = 'dead')
         THEN d.death_sprite ELSE rt.sprite END AS sprite,
    -- A gibbed corpse has a special frame (def. an 18+ game back in the day).
    CASE WHEN h.alive = FALSE AND (ai.state IS NULL OR ai.state = 'dead')
           THEN CASE WHEN h.health < -h.max_health AND d.xdeath_frame IS NOT NULL
                     THEN d.xdeath_frame ELSE d.death_frame END
         WHEN f.frame IS NOT NULL THEN f.frame
         ELSE rt.frame END AS frame,
    CASE WHEN h.alive = FALSE AND (ai.state IS NULL OR ai.state = 'dead')
           THEN d.death_fullbright
         WHEN f.frame IS NOT NULL THEN f.fullbright
         ELSE rt.fullbright END AS fullbright,
    CASE WHEN rt.spawn_ceiling THEN s.ceil_height - rt.thing_height
         -- a live floater is drawn where it hovers; a dead one has dropped
         WHEN d.floats AND COALESCE(h.alive, TRUE) THEN t.z
         ELSE s.floor_height END AS base_z,
    s.floor_height, s.ceil_height, s.light_level AS sector_light,
    (t.x - c.px) * COS(c.view_rad)
      + (t.y - c.py) * SIN(c.view_rad) AS depth,
    (t.x - c.px) * SIN(c.view_rad)
      - (t.y - c.py) * COS(c.view_rad) AS side,
    CAST((((CAST(FLOOR((
        DEGREES(ATAN2(t.y - c.py, t.x - c.px))
        - t.angle + rs.rotation_offset
      ) / rs.rotation_span) AS SIGNED) % 8) + 8) % 8 + 1) AS SIGNED) AS wanted_rotation,
    COALESCE(d.fuzzy, FALSE) AS fuzz
  FROM render_things rt
  CROSS JOIN render_context rc
  CROSS JOIN render_settings rs
  JOIN things t ON t.map_id = rt.map_id AND t.id = rt.thing_id
  LEFT JOIN thing_health h ON h.map_id = rt.map_id AND h.thing_id = rt.thing_id
  LEFT JOIN thing_combat_defs d ON d.thing_type = t.type
  -- Live AI state overrides the static spawn placement once a monster has
  -- been ticked (sector_id starts NULL and only gets set once it moves).
  LEFT JOIN monster_ai ai ON ai.map_id = rt.map_id AND ai.thing_id = rt.thing_id
  LEFT JOIN thing_ai_frames f ON f.thing_type = t.type AND f.state = ai.state
    AND f.seq_index = ai.seq_index
  -- Collected pickups stop rendering entirely
  LEFT JOIN picked_up_items pu ON pu.map_id = rt.map_id AND pu.thing_id = rt.thing_id
  JOIN sectors_lit s ON s.map_id = rt.map_id AND s.id = COALESCE(ai.sector_id, rt.sector_id)
  CROSS JOIN cam c
  WHERE rt.map_id = rc.map_id
    -- Single-player spawn filtering from Doom Thing options, gated by the
    -- client's selected difficulty (rc.skill_bit: 1/2/4, derived from the
    -- 0..4 skill).
    AND (t.flags & rc.skill_bit) <> 0
    AND (t.flags & 16) = 0
    AND pu.thing_id IS NULL
    -- Vanilla removes a barrel after BEXP E; other corpses remain visible.
    AND NOT (COALESCE(d.explodes, FALSE) AND ai.state='dead')
  UNION ALL
  SELECT
    -e.effect_id AS thing_id, e.x, e.y, 0,
    ed.sprite,
    -- One letter per tics_per_frame
    SUBSTRING(ed.frame_sequence
              FROM LEAST(LENGTH(ed.frame_sequence) - 1,
                         e.age / ed.tics_per_frame) + 1 FOR 1),
    ed.fullbright, e.z,
    s.floor_height, s.ceil_height, COALESCE(s.light_level, ps.light_level),
    (e.x - c.px) * COS(c.view_rad) + (e.y - c.py) * SIN(c.view_rad),
    (e.x - c.px) * SIN(c.view_rad) - (e.y - c.py) * COS(c.view_rad),
    0, FALSE
  FROM world_effects e
  CROSS JOIN render_context rc
  CROSS JOIN cam c
  CROSS JOIN player_sector ps
  JOIN effect_sprite_defs ed ON ed.effect_type = e.effect_type
  LEFT JOIN sectors_lit s ON s.map_id=e.map_id AND s.id=e.sector_id
  WHERE e.map_id=rc.map_id
  UNION ALL
  -- The other players of a deathmatch.
  -- A blur sphere makes them fuzzy like a spectre.
  SELECT
    CAST(ot.id AS SIGNED), ot.x, ot.y, ot.angle,
    'PLAY', op.sprite_frame, FALSE,
    s.floor_height, s.floor_height, s.ceil_height, s.light_level,
    (ot.x - c.px) * COS(c.view_rad) + (ot.y - c.py) * SIN(c.view_rad),
    (ot.x - c.px) * SIN(c.view_rad) - (ot.y - c.py) * COS(c.view_rad),
    CAST((((CAST(FLOOR((DEGREES(ATAN2(ot.y - c.py, ot.x - c.px))
        - ot.angle + rs.rotation_offset) / rs.rotation_span) AS SIGNED) % 8)
        + 8) % 8 + 1) AS SIGNED),
    (op.invis_tics > 0)
  FROM player_state op
  CROSS JOIN render_context rc
  CROSS JOIN render_settings rs
  JOIN things ot ON ot.map_id = op.map_id AND ot.id = op.player_thing_id
  JOIN sectors_lit s ON s.map_id = op.map_id AND s.id = op.sector_id
  CROSS JOIN cam c
  WHERE op.map_id = rc.map_id AND op.player_thing_id <> rc.player_thing_id
  UNION ALL
  SELECT
    -(1000000000000+mp.projectile_id) AS thing_id,
    mp.x,mp.y,CAST(DEGREES(ATAN2(mp.vy,mp.vx)) AS SIGNED),
    (CASE WHEN mp.state='fly' THEN pd.fly_sprite
          ELSE pd.impact_sprite END),
    CASE
      WHEN mp.projectile_type='rocket' THEN
        CASE WHEN mp.state='fly' THEN 'A' WHEN mp.age<8 THEN 'B'
             WHEN mp.age<14 THEN 'C' ELSE 'D' END
      WHEN mp.projectile_type='plasma' THEN
        CASE WHEN mp.state='fly' THEN CASE WHEN (mp.age%12)<6 THEN 'A' ELSE 'B' END
             WHEN mp.age<4 THEN 'A' WHEN mp.age<8 THEN 'B'
             WHEN mp.age<12 THEN 'C' WHEN mp.age<16 THEN 'D' ELSE 'E' END
      WHEN mp.projectile_type='bfg' THEN
        CASE WHEN mp.state='fly' THEN CASE WHEN (mp.age%8)<4 THEN 'A' ELSE 'B' END
             WHEN mp.age<8 THEN 'A' WHEN mp.age<16 THEN 'B'
             WHEN mp.age<24 THEN 'C' WHEN mp.age<32 THEN 'D'
             WHEN mp.age<40 THEN 'E' ELSE 'F' END
      ELSE CASE WHEN mp.state='fly'
             THEN CASE WHEN (mp.age%8)<4 THEN 'A' ELSE 'B' END
             WHEN mp.age<5 THEN 'C' WHEN mp.age<10 THEN 'D' ELSE 'E' END
    END,
    TRUE,mp.z,
    COALESCE(s.floor_height,ps.floor_height),
    COALESCE(s.ceil_height,ps.ceil_height),
    COALESCE(s.light_level,ps.light_level),
    (mp.x-c.px)*COS(c.view_rad)+(mp.y-c.py)*SIN(c.view_rad),
    (mp.x-c.px)*SIN(c.view_rad)-(mp.y-c.py)*COS(c.view_rad),
    CASE WHEN mp.state='fly' THEN
      CAST((((CAST(FLOOR((DEGREES(ATAN2(mp.y-c.py,mp.x-c.px))
          -DEGREES(ATAN2(mp.vy,mp.vx))+rs.rotation_offset)
          /rs.rotation_span) AS SIGNED)%8)+8)%8+1) AS SIGNED)
      ELSE 0 END, FALSE
  FROM monster_projectiles mp
  CROSS JOIN render_context rc
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
  CROSS JOIN player_sector ps
  JOIN projectile_defs pd ON pd.projectile_type = mp.projectile_type
  LEFT JOIN sectors_lit s ON s.map_id=mp.map_id AND s.id=mp.sector_id
  WHERE mp.map_id=rc.map_id
)
SELECT tv.thing_id, t.type, tv.x + 0e0 AS x, tv.y + 0e0 AS y, tv.base_z + 0e0 AS z, tv.angle + 0e0 AS angle, tv.sprite, tv.frame, tv.fullbright,
       tv.sector_light AS light, tv.fuzz,
       CASE WHEN h.alive = FALSE AND (ai.state IS NULL OR ai.state = 'dead') THEN 'dead'
            ELSE COALESCE(ai.state, 'stand') END AS state,
       COALESCE(h.alive, TRUE) AS alive
FROM thing_view tv
CROSS JOIN render_context rc
LEFT JOIN things t ON t.map_id = rc.map_id AND t.id = tv.thing_id
LEFT JOIN monster_ai ai ON ai.map_id = rc.map_id AND ai.thing_id = tv.thing_id
LEFT JOIN thing_health h ON h.map_id = rc.map_id AND h.thing_id = tv.thing_id
WHERE tv.depth >= -128
