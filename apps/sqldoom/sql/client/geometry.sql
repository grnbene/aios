-- The 3D renderer's geometry: one frame's triangles in camera space, for r3d_draw_tex (play.gr). SQLDoom's map,
-- sectors, things and effects are read as the game tic leaves them; the renderer is no longer the 320x200 software
-- one (sql/renderer.sql) but a vertex stage: walls, floors, ceilings and sprites placed, culled and lit here, and
-- filled at the window's own size in true colour by the native rasterizer.
--
-- Reads @map_id, @player_thing_id, @skill (0..4), the camera @x @y @z (eye) and @angle (degrees, Doom's: 0 east,
-- 90 north) and @tic (the gameplay clock, for animated lights, flats and scrolling walls). One row a triangle:
-- x0 y0 z0 u0 v0 .. x2 y2 z2 u2 v2 (points on the map: x east, y north, z up; u v in texels), tex
-- (a texture id play.gr gave r3d_texture: walls their tex_id, flats 10000 + flat_id, sprites 20000 + lump_id),
-- light (0..255) and flags (1 masked, 2 full bright, 4 clamp v, 8 fuzz).
--
-- Doom's rules kept: a seg is drawn from its front only; a two-sided line has a lower wall where the floor behind
-- is higher, an upper wall where the ceiling behind is lower (none between two skies) and a masked middle that
-- is not repeated up or down; upper and lower unpegging; walls along the x axis a light step darker, along y one
-- brighter; flats repeat every 64 units on the map's grid; a sky ceiling is not drawn (the sky is behind it).
-- The sprites are SQLDoom's own choice of each Thing's frame (renderer.sql's thing_view, kept as it is there).
--
-- It is worked out once a game tic, and each frame until the next turns the same points to the camera where it is
-- then (r3d_draw_tex's camera): so what is culled is culled with room to spare (a wall or a floor is dropped only
-- when it is well behind the eye), and a sprite faces the eye as it was at the tic.
WITH RECURSIVE render_context AS (
    SELECT @map_id AS map_id, @player_thing_id AS player_thing_id, @skill AS skill,
           CASE WHEN @skill<=1 THEN 1 WHEN @skill=2 THEN 2 ELSE 4 END AS skill_bit
), frame_clock AS (
    SELECT CAST(@tic AS SIGNED) AS t
), render_settings AS (
    SELECT 202.5e0 AS rotation_offset, 45e0 AS rotation_span
), cam AS (
    SELECT @x AS px, @y AS py, @z AS pz, radians(@angle) AS view_rad,
           COS(radians(@angle)) AS ca, SIN(radians(@angle)) AS sa
), animated_light AS (
    -- Doom hangs a light thinker off every animated sector. Here the effect is
    -- a pure function of the tic count and the two endpoints resolved at level
    -- start into sector_light_fx, so it is evaluated for the frame being drawn.
    SELECT k.sector_id,
      CAST(LEAST(255, GREATEST(0,
    CASE k.special
      -- Strobes: 5 tics bright, then 15 (fast) or 35 (slow) dark.
      WHEN 2  THEN CASE WHEN ((k.t + k.phase) % 20) < 5 THEN k.base_light ELSE k.dark_light END
      WHEN 13 THEN CASE WHEN (k.t % 20) < 5 THEN k.base_light ELSE k.dark_light END
      WHEN 3  THEN CASE WHEN ((k.t + k.phase) % 40) < 5 THEN k.base_light ELSE k.dark_light END
      WHEN 12 THEN CASE WHEN (k.t % 40) < 5 THEN k.base_light ELSE k.dark_light END
      -- Irregular flicker
      WHEN 1  THEN CASE WHEN ((k.sector_id*2654435761 + (k.t DIV 4)) % 8) < 2
                        THEN k.dark_light ELSE k.base_light END
      -- Fire flicker
      WHEN 17 THEN GREATEST(k.dark_light,
                     k.base_light - CAST(((((((k.t DIV 3) % 1000) * (((k.t DIV 3) % 1000))
                                        * 1103515245) + k.sector_id * 40503)
                                      % 1009) % 4
                       * GREATEST(1, (k.base_light - k.dark_light) DIV 4)) AS SIGNED))
      -- Glow
      WHEN 8  THEN (k.dark_light + 8 * (
                      CASE WHEN ((k.t + k.phase) % (2*k.glow_steps)) < k.glow_steps
                           THEN ((k.t + k.phase) % (2*k.glow_steps))
                           ELSE 2*k.glow_steps - ((k.t + k.phase) % (2*k.glow_steps))
                      END))
      ELSE k.base_light
    END
      )) AS SIGNED) AS light_level
    FROM (
      SELECT f.sector_id, f.base_light, f.dark_light, f.special,
             (f.sector_id*7919) % 64 AS phase,
             GREATEST(1, (f.base_light - f.dark_light) DIV 8) AS glow_steps,
             fc.t
      FROM sector_light_fx f
      CROSS JOIN render_context rc
      CROSS JOIN frame_clock fc
      WHERE f.map_id = rc.map_id
    ) k

), sectors_lit AS (
    SELECT s.map_id, s.id, s.floor_height, s.ceil_height, s.floor_tex, s.ceil_tex,
           COALESCE(a.light_level, s.light_level) AS light_level
    FROM sectors s
    CROSS JOIN render_context rc
    LEFT JOIN animated_light a ON a.sector_id = s.id
    WHERE s.map_id = rc.map_id
), player_sector AS (
    SELECT s.floor_height, s.ceil_height, s.light_level
    FROM player_state p
    CROSS JOIN render_context rc
    JOIN sectors_lit s ON s.id = p.sector_id
    WHERE p.map_id = rc.map_id AND p.player_thing_id = rc.player_thing_id
),
-- Doom's animated flats: each member of a cycle shows the cycle's next member every 8 tics.
flat_anim (name, grp, pos, cnt) AS (
    SELECT 'NUKAGE1', 1, 0, 3 UNION ALL SELECT 'NUKAGE2', 1, 1, 3 UNION ALL SELECT 'NUKAGE3', 1, 2, 3
    UNION ALL SELECT 'FWATER1', 2, 0, 4 UNION ALL SELECT 'FWATER2', 2, 1, 4 UNION ALL SELECT 'FWATER3', 2, 2, 4
    UNION ALL SELECT 'FWATER4', 2, 3, 4
    UNION ALL SELECT 'LAVA1', 3, 0, 4 UNION ALL SELECT 'LAVA2', 3, 1, 4 UNION ALL SELECT 'LAVA3', 3, 2, 4
    UNION ALL SELECT 'LAVA4', 3, 3, 4
    UNION ALL SELECT 'BLOOD1', 4, 0, 3 UNION ALL SELECT 'BLOOD2', 4, 1, 3 UNION ALL SELECT 'BLOOD3', 4, 2, 3
    UNION ALL SELECT 'SWATER1', 5, 0, 4 UNION ALL SELECT 'SWATER2', 5, 1, 4 UNION ALL SELECT 'SWATER3', 5, 2, 4
    UNION ALL SELECT 'SWATER4', 5, 3, 4
    UNION ALL SELECT 'SLIME01', 6, 0, 4 UNION ALL SELECT 'SLIME02', 6, 1, 4 UNION ALL SELECT 'SLIME03', 6, 2, 4
    UNION ALL SELECT 'SLIME04', 6, 3, 4
),
-- Each seg seen from its front, with both sectors.
seg_front AS (
    SELECT sg.id, v1.x AS x1, v1.y AS y1, v2.x AS x2, v2.y AS y2,
           sg.offs + fsd.x_offset + CASE WHEN ld.scrolls THEN fc.t ELSE 0 END AS u_start,
           SQRT((v2.x - v1.x) * (v2.x - v1.x) + (v2.y - v1.y) * (v2.y - v1.y)) AS len,
           ld.flags, fsd.y_offset, fsd.upper_tex, fsd.lower_tex, fsd.mid_tex,
           fsd.sector_id AS fsid, bsd.sector_id AS bsid
    FROM segs sg
    CROSS JOIN render_context rc
    CROSS JOIN cam c
    CROSS JOIN frame_clock fc
    JOIN vertexes v1 ON v1.map_id = sg.map_id AND v1.id = sg.v1_id
    JOIN vertexes v2 ON v2.map_id = sg.map_id AND v2.id = sg.v2_id
    JOIN linedefs ld ON ld.map_id = sg.map_id AND ld.id = sg.linedef_id
    JOIN sidedefs fsd ON fsd.map_id = sg.map_id
      AND fsd.id = CASE WHEN sg.direction = 0 THEN ld.right_sd_id ELSE ld.left_sd_id END
    LEFT JOIN sidedefs bsd ON bsd.map_id = sg.map_id
      AND bsd.id = CASE WHEN sg.direction = 0 THEN ld.left_sd_id ELSE ld.right_sd_id END
    WHERE sg.map_id = rc.map_id
      -- the camera on the seg's right, its front
      AND (c.px - v1.x) * (v2.y - v1.y) - (c.py - v1.y) * (v2.x - v1.x) > 0
      -- not wholly well behind the eye (the frames until the next tic turn it)
      AND ((v1.x - c.px) * c.ca + (v1.y - c.py) * c.sa > -96 OR (v2.x - c.px) * c.ca + (v2.y - c.py) * c.sa > -96)
),
-- The walls: kind 0 a one-sided middle, 1 a lower, 2 an upper, 3 a two-sided (masked) middle, each with its
-- texture's top row at zt (v_top) and its flags.
wall_raw AS (
    SELECT s.*, 0 AS kind, s.mid_tex AS tname, f.floor_height AS zb, f.ceil_height AS zt,
           f.ceil_height AS fch, f.floor_height AS ffh, f.floor_height AS bfh, f.ceil_height AS bch, f.light_level AS light
    FROM seg_front s JOIN sectors_lit f ON f.id = s.fsid
    WHERE s.bsid IS NULL AND s.mid_tex <> '-'
    UNION ALL
    SELECT s.*, 1, s.lower_tex, f.floor_height, b.floor_height,
           f.ceil_height, f.floor_height, b.floor_height, b.ceil_height, f.light_level
    FROM seg_front s JOIN sectors_lit f ON f.id = s.fsid JOIN sectors_lit b ON b.id = s.bsid
    WHERE b.floor_height > f.floor_height AND s.lower_tex <> '-'
    UNION ALL
    SELECT s.*, 2, s.upper_tex, b.ceil_height, f.ceil_height,
           f.ceil_height, f.floor_height, b.floor_height, b.ceil_height, f.light_level
    FROM seg_front s JOIN sectors_lit f ON f.id = s.fsid JOIN sectors_lit b ON b.id = s.bsid
    WHERE b.ceil_height < f.ceil_height AND s.upper_tex <> '-'
      AND NOT (f.ceil_tex = 'F_SKY1' AND b.ceil_tex = 'F_SKY1')
    UNION ALL
    SELECT s.*, 3, s.mid_tex, GREATEST(f.floor_height, b.floor_height), LEAST(f.ceil_height, b.ceil_height),
           f.ceil_height, f.floor_height, b.floor_height, b.ceil_height, f.light_level
    FROM seg_front s JOIN sectors_lit f ON f.id = s.fsid JOIN sectors_lit b ON b.id = s.bsid
    WHERE s.mid_tex <> '-'
),
wall_tex AS (
    SELECT w.*, wm.tex_id, wm.height AS th,
           -- the world height of the texture's top row, for a masked middle (it is drawn once, not repeated)
           CASE WHEN (w.flags & 16) <> 0 THEN w.zb + wm.height + w.y_offset ELSE w.zt + w.y_offset END AS mtop
    FROM wall_raw w JOIN walltex_meta wm ON wm.name = w.tname
),
walls AS (
    SELECT w.x1, w.y1, w.x2, w.y2, w.u_start, w.len, w.tex_id,
           CASE WHEN w.kind = 3 THEN LEAST(w.zt, w.mtop) ELSE w.zt END AS top_z,
           CASE WHEN w.kind = 3 THEN GREATEST(w.zb, w.mtop - w.th) ELSE w.zb END AS bot_z,
           CASE w.kind
             WHEN 0 THEN CASE WHEN (w.flags & 16) <> 0 THEN w.th - (w.zt - w.zb) ELSE 0 END + w.y_offset
             WHEN 1 THEN CASE WHEN (w.flags & 16) <> 0 THEN w.fch - w.bfh ELSE 0 END + w.y_offset
             WHEN 2 THEN CASE WHEN (w.flags & 8) <> 0 THEN 0 ELSE w.th - (w.fch - w.bch) END + w.y_offset
             ELSE w.mtop - LEAST(w.zt, w.mtop) END AS v_top,
           LEAST(255, GREATEST(0, w.light + CASE WHEN w.y1 = w.y2 THEN -16 WHEN w.x1 = w.x2 THEN 16 ELSE 0 END)) AS light,
           CASE WHEN w.kind = 3 THEN 5 ELSE 0 END AS flags
    FROM wall_tex w
),
-- Each wall's two ends and heights.
wall_w AS (
    SELECT w.x1 AS ax, w.y1 AS ay, w.x2 AS bx, w.y2 AS `by`, w.bot_z AS zb, w.top_z AS zt,
           w.u_start AS ua, w.u_start + w.len AS ub, w.v_top AS vt, w.v_top + (w.top_z - w.bot_z) AS vb,
           w.tex_id, w.light, w.flags
    FROM walls w
    WHERE w.top_z > w.bot_z
),
-- Floors and ceilings: each subsector's triangles at its sector's heights (as the tic leaves them), the flats'
-- texture the animated one for this tic.
flat_raw AS (
    SELECT t.x0, t.y0, t.x1, t.y1, t.x2, t.y2, p.plane, s.light_level AS light,
           CASE WHEN p.plane = 0 THEN s.floor_height ELSE s.ceil_height END AS z,
           CASE WHEN p.plane = 0 THEN s.floor_tex ELSE s.ceil_tex END AS tname
    FROM ssector_tris t
    CROSS JOIN render_context rc
    CROSS JOIN cam c
    JOIN sectors_lit s ON s.id = t.sector_id
    CROSS JOIN (SELECT 0 AS plane UNION ALL SELECT 1) p
    WHERE t.map_id = rc.map_id
      AND ((p.plane = 0 AND c.pz > s.floor_height) OR (p.plane = 1 AND c.pz < s.ceil_height AND s.ceil_tex <> 'F_SKY1'))
      AND ((t.x0 - c.px) * c.ca + (t.y0 - c.py) * c.sa > -96 OR (t.x1 - c.px) * c.ca + (t.y1 - c.py) * c.sa > -96
           OR (t.x2 - c.px) * c.ca + (t.y2 - c.py) * c.sa > -96)
),
flat_w AS (
    SELECT f.x0, f.y0, f.x1, f.y1, f.x2, f.y2, f.z, 10000 + fl.flat_id AS tex, f.light
    FROM flat_raw f
    CROSS JOIN frame_clock fc
    LEFT JOIN flat_anim a ON a.name = f.tname
    LEFT JOIN flat_anim a2 ON a2.grp = a.grp AND a2.pos = (a.pos + fc.t DIV 8) % a.cnt
    JOIN flat_textures fl ON fl.name = COALESCE(a2.name, f.tname)
),
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
),
thing_pick AS (
  -- the frame for the rotation wanted, else the one drawn from every side (rotation 0)
  SELECT r.*
  FROM (
    SELECT tv.*, sl.lump_id, sf.flipped, sf.width, sf.height, sf.left_offset, sf.top_offset,
           ROW_NUMBER() OVER (PARTITION BY tv.thing_id
                              ORDER BY CASE WHEN sf.rotation = tv.wanted_rotation THEN 0 ELSE 1 END) AS rot_rank
    FROM thing_view tv
    JOIN sprite_frames sf ON sf.sprite = tv.sprite AND sf.frame = tv.frame AND sf.rotation IN (0, tv.wanted_rotation)
    JOIN sprite_lumps sl ON sl.lump_name = sf.lump_name
    WHERE tv.depth >= -96
  ) r
  WHERE r.rot_rank = 1
),
-- A sprite is a picture standing upright facing the eye: its quad across the line of sight, on the map.
sprite_w AS (
  SELECT tp.x + c.sa * (-tp.left_offset) AS lx, tp.y - c.ca * (-tp.left_offset) AS ly,
         tp.x + c.sa * (tp.width - tp.left_offset) AS rx, tp.y - c.ca * (tp.width - tp.left_offset) AS ry,
         tp.base_z + tp.top_offset - tp.height AS zb, tp.base_z + tp.top_offset AS zt,
         CASE WHEN tp.flipped THEN tp.width ELSE 0 END AS ul, CASE WHEN tp.flipped THEN 0 ELSE tp.width END AS ur,
         tp.height AS vb, 20000 + tp.lump_id AS tex, tp.sector_light AS light,
         5 + CASE WHEN tp.fullbright THEN 2 ELSE 0 END + CASE WHEN tp.fuzz THEN 8 ELSE 0 END AS flags
  FROM thing_pick tp CROSS JOIN cam c
),
quad_half (h) AS (SELECT 0 UNION ALL SELECT 1)
-- A quad (walls, sprites) is two triangles: (A B C) and (A C D) of its corners A (start, bottom), B (end, bottom),
-- C (end, top), D (start, top).
SELECT w.ax AS x0, w.ay AS y0, w.zb AS z0, w.ua AS u0, w.vb AS v0,
       w.bx AS x1, w.`by` AS y1, CASE WHEN q.h = 0 THEN w.zb ELSE w.zt END AS z1,
       w.ub AS u1, CASE WHEN q.h = 0 THEN w.vb ELSE w.vt END AS v1,
       CASE WHEN q.h = 0 THEN w.bx ELSE w.ax END AS x2, CASE WHEN q.h = 0 THEN w.`by` ELSE w.ay END AS y2, w.zt AS z2,
       CASE WHEN q.h = 0 THEN w.ub ELSE w.ua END AS u2, w.vt AS v2,
       w.tex_id AS tex, w.light, w.flags
FROM wall_w w CROSS JOIN quad_half q
UNION ALL
SELECT f.x0, f.y0, f.z, f.x0, -f.y0, f.x1, f.y1, f.z, f.x1, -f.y1, f.x2, f.y2, f.z, f.x2, -f.y2, f.tex, f.light, 0
FROM flat_w f
UNION ALL
SELECT s.lx, s.ly, s.zb, s.ul, s.vb,
       s.rx, s.ry, CASE WHEN q.h = 0 THEN s.zb ELSE s.zt END, s.ur, CASE WHEN q.h = 0 THEN s.vb ELSE 0 END,
       CASE WHEN q.h = 0 THEN s.rx ELSE s.lx END, CASE WHEN q.h = 0 THEN s.ry ELSE s.ly END, s.zt,
       CASE WHEN q.h = 0 THEN s.ur ELSE s.ul END, 0,
       s.tex, s.light, s.flags
FROM sprite_w s CROSS JOIN quad_half q
