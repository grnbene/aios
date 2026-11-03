-- SQLDoom's frame renderer (github.com/cedardb/sqldoom, sql/renderer.sql, GPL-2.0) in MySQL's dialect for ASQL.
-- One statement: the caller sets @map_id, @player_thing_id, @skill (0..4) and the camera @x, @y, @z, @angle (the
-- prepared statement's $1..$7 in SQLDoom), and gets one row, one column: the 320x200 frame as 192,000 RGB bytes.
--
-- Where this differs from SQLDoom, and why:
--  * casts are CAST(.. AS SIGNED / CHAR); PostgreSQL's integer `/` is DIV (MySQL's `/` never truncates);
--  * texture, flat and sprite texels are rows (walltex_texels, flat_texels, sprite_texels) joined on (name, u, v),
--    where SQLDoom reads GET_BYTE out of one BYTEA per texture: a row of ASQL's carries its values as text, so a
--    texture's bytes would be copied into every pixel's row on the way;
--  * a top-1 LEFT JOIN LATERAL (the nearest earlier panel with a ceiling or floor above / below the eye, a Thing's
--    sprite rotation) is a window function and a join, and lateral column aliases (render_settings) a derived table;
--  * BOOL_AND is MIN of the booleans, ARG_MAX a ROW_NUMBER, string_agg GROUP_CONCAT(.. SEPARATOR ''), || CONCAT,
--    IS DISTINCT FROM `NOT (a <=> b)`, and the psprite/view FULL JOIN a LEFT JOIN plus the psprite pixels it misses.
WITH RECURSIVE render_context AS (
    SELECT @map_id AS map_id,
           @player_thing_id AS player_thing_id,
           -- Doom's 0..4 skill
           @skill AS skill,
           CASE WHEN @skill<=1 THEN 1 WHEN @skill=2 THEN 2 ELSE 4 END
             AS skill_bit
), pos AS (
    -- The camera pose is passed in rather than read from the player Thing, so
    -- the client can render intermediate poses between the 35 Hz gameplay tics.
    SELECT @x AS x, @y AS y, @z AS z, @angle AS angle
), frame_clock AS (
    -- The gameplay clock, for the effects below that are pure functions of it.
    SELECT CAST(COALESCE(MAX(ps.level_tics), 0) AS SIGNED) AS t
    FROM player_state ps CROSS JOIN render_context rc
    WHERE ps.map_id = rc.map_id AND ps.player_thing_id = rc.player_thing_id
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
    -- sectors with the animated light applied
    SELECT s.map_id, s.id, s.floor_height, s.ceil_height,
           s.spawn_floor_height, s.spawn_ceil_height, s.spawn_floor_tex,
           s.spawn_light_level, s.floor_tex, s.ceil_tex,
           COALESCE(a.light_level, s.light_level) AS light_level,
           s.special, s.tag
    FROM sectors s
    CROSS JOIN render_context rc
    LEFT JOIN animated_light a ON a.sector_id = s.id
    WHERE s.map_id = rc.map_id
), scrolling_segs AS (
    -- Special 48 scrolls its wall one unit a tic
    SELECT ld.id AS linedef_id
    FROM linedefs ld CROSS JOIN render_context rc
    WHERE ld.map_id = rc.map_id AND ld.scrolls
), render_settings AS (
    -- The settings that read other settings are worked out from them in the outer SELECT (CedarDB lets a SELECT
    -- name its own aliases; MySQL does not).
    SELECT b.*,
      (b.screen_w/2e0) AS cx,
      (b.screen_h/2e0) AS cy,
      (b.screen_w/2e0)/tan(b.fov_rad/2e0) AS focal,
      tan(b.fov_rad/2e0)   AS tan_half_fov,
      -- A flat is 64 world units across, so once one screen pixel steps more
      -- than a quarter of that the surface stops resembling its texture and
      -- turns to noise. The horizontal step of a plane is depth/focal, so this
      -- caps a plane's drawable depth.
      16e0 * ((b.screen_w/2e0)/tan(b.fov_rad/2e0)) AS max_plane_depth
    FROM (SELECT
      320  AS screen_w,
      COALESCE(@screen_h, 200) AS screen_h,
      radians(90e0) AS fov_rad,
      1e-3 AS near,
      202.5e0 AS rotation_offset,
      45e0  AS rotation_span,
      32 AS light_index_invuln,
      23 AS light_psprite_bias,
      256e0 AS sky_columns,
      90e0  AS sky_degrees,
      128      AS sky_rows,
      100      AS sky_horizon_y,
      64 AS flat_size) b
), cam AS (
    SELECT
    pos.x as px,
    pos.y as py,
    pos.z as pz,  -- eye height above floor
    radians(pos.angle) as view_rad
    FROM pos
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
-- Conservative view-frustum test per node_children row. A child is skipped only when ALL FOUR of its
-- bbox corners lie strictly outside one of the 3 view half-planes (near clip, left FOV edge, right FOV edge).
visible_children AS (
  SELECT
    nc.map_id, nc.node_id, nc.side,
    (c.px BETWEEN nc.bbox_left AND nc.bbox_right
     AND c.py BETWEEN nc.bbox_bottom AND nc.bbox_top)
    OR (
      -- MIN over the box of L = vy - vx*tan(halfFOV); keep unless proven
      -- fully outside the left edge.
      ((SIN(-c.view_rad) - rs.tan_half_fov*COS(-c.view_rad))
         * (CASE WHEN (SIN(-c.view_rad) - rs.tan_half_fov*COS(-c.view_rad)) >= 0
                 THEN nc.bbox_left ELSE nc.bbox_right END)
       + (COS(-c.view_rad) + rs.tan_half_fov*SIN(-c.view_rad))
         * (CASE WHEN (COS(-c.view_rad) + rs.tan_half_fov*SIN(-c.view_rad)) >= 0
                 THEN nc.bbox_bottom ELSE nc.bbox_top END)
       + (-c.px*SIN(-c.view_rad) - c.py*COS(-c.view_rad)
          + rs.tan_half_fov*(c.px*COS(-c.view_rad) - c.py*SIN(-c.view_rad)))
      ) <= 0.5
      AND
      -- MIN over the box of R = -vy - vx*tan(halfFOV); right edge.
      ((-SIN(-c.view_rad) - rs.tan_half_fov*COS(-c.view_rad))
         * (CASE WHEN (-SIN(-c.view_rad) - rs.tan_half_fov*COS(-c.view_rad)) >= 0
                 THEN nc.bbox_left ELSE nc.bbox_right END)
       + (-COS(-c.view_rad) + rs.tan_half_fov*SIN(-c.view_rad))
         * (CASE WHEN (-COS(-c.view_rad) + rs.tan_half_fov*SIN(-c.view_rad)) >= 0
                 THEN nc.bbox_bottom ELSE nc.bbox_top END)
       + (c.px*SIN(-c.view_rad) + c.py*COS(-c.view_rad)
          + rs.tan_half_fov*(c.px*COS(-c.view_rad) - c.py*SIN(-c.view_rad)))
      ) <= 0.5
      AND
      -- MIN over the box of N = near - vx; near clip plane.
      ((-COS(-c.view_rad))
         * (CASE WHEN (-COS(-c.view_rad)) >= 0 THEN nc.bbox_left ELSE nc.bbox_right END)
       + (SIN(-c.view_rad))
         * (CASE WHEN SIN(-c.view_rad) >= 0 THEN nc.bbox_bottom ELSE nc.bbox_top END)
       + (rs.near + c.px*COS(-c.view_rad) - c.py*SIN(-c.view_rad))
      ) <= 0.5
    ) AS keep
  FROM node_children nc
  CROSS JOIN cam c
  CROSS JOIN render_settings rs
  CROSS JOIN render_context rctx
  WHERE nc.map_id = rctx.map_id
),
-- Front-to-back order over subsectors.
-- A nice property of Doom is that the BSP tree tells us exactly in which order we have
-- to render sectors without having to worry about overlaps - just start rendering at the first node
-- and continue filling pixels that haven't been set yet, until the bsp tree is fully traversed - divide and conquer!
-- We just have to traverse the bsp tree to our sector and record the front/back choices
-- along the way.
-- We cheat a little bit: SQL does set-oriented processing, which makes traversal expensive
-- and hard to express. So we just pre-compute all paths in node_path_steps.
-- And aggregate over that (~6k rows).
-- We then pack the choices we made into one bigint: back = 1 at bit (40 - depth), front = 0.
-- By sorting the keys lexicographically, we get an order in which to render segments.
-- We also apply pruning: A subsector under any culled child box is left out from further steps.
-- Sounds a little bit like cheating but even John Carmack himself set in 1993 that he is sure
-- there's going to be a future where rendering DOOM can just be brute-forced. Not sure if he
-- had SQL in mind, though.
bsp_order AS (
  SELECT s.ssector_id, ROW_NUMBER() OVER (ORDER BY s.sort_key) AS bsp_seq
  FROM (
    SELECT st.ssector_id,
           -- The side of the partition line the camera is on is the front.
           SUM(CASE WHEN st.side = CASE
                      WHEN CAST((p.x - n.x) AS SIGNED) * CAST(n.dy AS SIGNED) - CAST((p.y - n.y) AS SIGNED) * CAST(n.dx AS SIGNED) > 0
                        THEN 'R' ELSE 'L' END
                    THEN 0
                    ELSE (1 << (40 - st.depth)) END) AS sort_key,
           MIN(vc.keep) AS visible   -- BOOL_AND
    FROM node_path_steps st
    CROSS JOIN render_context rc
    JOIN nodes n ON n.map_id = st.map_id AND n.id = st.node_id
    CROSS JOIN pos p
    JOIN visible_children vc ON vc.map_id = st.map_id AND vc.node_id = st.node_id
      AND vc.side = st.side
    WHERE st.map_id = rc.map_id
    GROUP BY st.ssector_id
  ) s
  WHERE s.visible
),
player_sector AS (
  -- The first subsector in the front-to-back walk contains the camera.
  SELECT sec.*
  FROM bsp_order bo
  JOIN render_context rc ON TRUE
  JOIN render_segs sg ON sg.map_id = rc.map_id
    AND sg.ssector_id = bo.ssector_id
  JOIN sectors_lit sec ON sec.map_id = rc.map_id AND sec.id = sg.fsec
  WHERE bo.bsp_seq = 1
  ORDER BY sg.seg_id
  LIMIT 1
),
segs_with_verts AS (
  SELECT
    s.seg_id,
    s.direction,
    s.linedef_id,
    s.map_id,
    s.x1, s.y1, s.x2, s.y2,
    s.seg_u1, s.seg_u2
  FROM render_segs s
  -- Only segs the BSP walk actually reached
  JOIN bsp_order bo ON bo.ssector_id = s.ssector_id
  CROSS JOIN pos
  CROSS JOIN render_context rc
  WHERE s.map_id = rc.map_id
    -- A Doom seg is directed so that its front sector lies on its right.
    -- Two-sided linedefs normally occur twice in SEGS, once per subsector and
    -- in opposite directions. Keep only the copy facing the camera.
    AND CAST((s.x2 - s.x1) AS SIGNED) * CAST((pos.y - s.y1) AS SIGNED)
      - CAST((s.y2 - s.y1) AS SIGNED) * CAST((pos.x - s.x1) AS SIGNED) < 0
),
viewspace AS (
    -- translate to camera origin and rotate by -view_rad in one step.
    SELECT
        s.seg_id,
        s.direction,
        s.linedef_id,
        s.map_id,
        s.seg_u1, s.seg_u2,
        (s.x1 - c.px) * cos(-c.view_rad) - (s.y1 - c.py) * sin(-c.view_rad) AS x1,
        -((s.x1 - c.px) * sin(-c.view_rad) + (s.y1 - c.py) * cos(-c.view_rad)) AS y1,
        (s.x2 - c.px) * cos(-c.view_rad) - (s.y2 - c.py) * sin(-c.view_rad) AS x2,
        -((s.x2 - c.px) * sin(-c.view_rad) + (s.y2 - c.py) * cos(-c.view_rad)) AS y2
    FROM segs_with_verts s CROSS JOIN cam c
),
visible AS (
    SELECT * FROM viewspace WHERE x1 > 0 OR x2 > 0
),
clipped AS (
    SELECT
        seg_id,
        direction,
        linedef_id,
        map_id,
        seg_u1,
        seg_u2,
        CASE WHEN x1 < near then near ELSE x1 END AS cx1,
        CASE WHEN x1 < near THEN y1 + (near - x1)*(y2 - y1)/(x2 - x1) ELSE y1 END AS cy1,
        CASE WHEN x2 < near then near ELSE x2 END AS cx2,
        CASE WHEN x2 < near THEN y2 + (near - x2)*(y1 - y2)/(x1 - x2) ELSE y2 END AS cy2
    FROM visible, render_settings
),
projected AS (
    SELECT
        seg_id,
        direction,
        linedef_id,
        map_id,
        cx1, cy1, cx2, cy2,
        cx + focal*(cy1/cx1) AS screen_x1,
        cx + focal*(cy2/cx2) AS screen_x2,
        1.0/cx1 AS invx1,
        1.0/cx2 AS invx2,
        seg_u1,
        seg_u2
    FROM clipped, render_settings
),
on_screen AS (
  -- Frustum reject. A segment is retained when its clipped projected interval overlaps any part of the screen.
  -- this also keeps segments whose two endpoints lie outside opposite edges (ask me how I found this out the hard way).
  SELECT p.*
  FROM projected p
  CROSS JOIN render_settings rs
  WHERE GREATEST(p.screen_x1, p.screen_x2) >= 0
    AND LEAST(p.screen_x1, p.screen_x2) < rs.screen_w
),
heights AS (
  SELECT
    p.*, s.fsec, s.bsec,
    (s.x_offset + CASE WHEN sc.linedef_id IS NULL THEN 0 ELSE fc.t END) AS x_offset,
    s.y_offset,
    s.upper_tex, s.mid_tex, s.lower_tex, s.flags,
    s.f_floor, s.f_ceil, s.f_ceil_tex,
    -- f_light/b_light are the sector's static light, an animated sector's value is computed above
    COALESCE(alf.light_level, s.f_light) AS f_light,
    s.b_floor, s.b_ceil, s.b_ceil_tex,
    COALESCE(alb.light_level, s.b_light) AS b_light
  FROM on_screen p
  JOIN render_segs s ON s.map_id = p.map_id AND s.seg_id + 0 = p.seg_id -- psst, optimizer hack
  CROSS JOIN frame_clock fc
  LEFT JOIN animated_light alf ON alf.sector_id = s.fsec
  LEFT JOIN animated_light alb ON alb.sector_id = s.bsec
  LEFT JOIN scrolling_segs sc ON sc.linedef_id = s.linedef_id
),
seg_light_bias AS (
  -- Doom gives horizontal walls one darker light step and vertical
  -- walls one brighter step, so edges/corners are easier to see
  SELECT s.seg_id + 0 AS seg_id, s.light_bias -- psst, optimizer hack
  FROM render_segs s
  CROSS JOIN render_context rc
  WHERE s.map_id = rc.map_id
),
seg_bsp AS (
  -- which subsector (and thus bsp_seq) each seg belongs to
  SELECT s.seg_id + 0 AS seg_id, bo.bsp_seq
  FROM render_segs s
  JOIN bsp_order bo ON bo.ssector_id = s.ssector_id
  CROSS JOIN render_context rc
  WHERE s.map_id = rc.map_id
),
occluders AS (
  -- segs that fully block anything farther
  -- one-sided walls, or two-sided walls whose back sector is degenerate
  -- (closed door / dummy sector)
  SELECT h.seg_id, h.screen_x1, h.screen_x2, sb.bsp_seq
  FROM heights h
  JOIN seg_bsp sb ON sb.seg_id = h.seg_id
  WHERE h.bsec IS NULL OR h.b_ceil <= h.b_floor
),
occlusion_cols AS (
  SELECT x AS col_x, o.bsp_seq
  FROM occluders o
  CROSS JOIN render_settings rs
  CROSS JOIN LATERAL generate_series(
    GREATEST(0, CAST(FLOOR(LEAST(o.screen_x1, o.screen_x2)) AS SIGNED)),
    LEAST(rs.screen_w - 1, CAST(CEIL(GREATEST(o.screen_x1, o.screen_x2)) AS SIGNED))
  ) AS x
  WHERE o.screen_x1 <> o.screen_x2
),
min_occlusion AS (
  -- nearest (smallest bsp_seq) fully-solid occluder covering each column;
  -- anything with a strictly larger bsp_seq in that column is behind it and
  -- can be ignored
  SELECT col_x, MIN(bsp_seq) AS min_bsp_seq
  FROM occlusion_cols
  GROUP BY col_x
),
wall_parts AS (
  -- SOLID (one-sided)
  SELECT seg_id, 'solid' AS part, mid_tex AS tex, x_offset, y_offset, flags,
         f_floor AS z_bot, f_ceil AS z_top,
         CASE WHEN (flags & 16)<>0 THEN f_floor ELSE f_ceil END AS v_anchor_base,
         (flags & 16)<>0 AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NULL
  AND f_ceil > f_floor            -- skip zero/inverted-height (dummy) sectors

  UNION ALL
  -- UPPER (two-sided, when back ceiling < front ceiling)
  SELECT seg_id, 'upper', upper_tex, x_offset, y_offset, flags,
         b_ceil AS z_bot, f_ceil AS z_top,
         CASE WHEN (flags & 8)<>0 THEN f_ceil ELSE b_ceil END AS v_anchor_base,
         (flags & 8)=0 AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND b_ceil < f_ceil
  -- Doom joins adjacent sky ceilings visually even when their numeric heights
  -- differ. Drawing this upper wall creates a fake floating overhang whose
  -- "underside" is (correctly) still sky.
  AND (NOT (f_ceil_tex <=> 'F_SKY1')
       OR NOT (b_ceil_tex <=> 'F_SKY1'))

  UNION ALL
  -- CEILING OPEN-UP (two-sided, back ceiling at or above the front ceiling).
  -- There is no wall surface on the camera-facing side, but the front ceiling
  -- ends at this portal. Keep it as a clip-only boundary.
  SELECT seg_id, 'upper_open', NULL, x_offset, y_offset, flags,
         f_ceil AS z_bot, b_ceil AS z_top,
         b_ceil AS v_anchor_base,FALSE AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND b_ceil > f_ceil
  AND (NOT (f_ceil_tex <=> 'F_SKY1')
       OR NOT (b_ceil_tex <=> 'F_SKY1'))

  UNION ALL
  -- CEILING FLUSH BOUNDARY (two-sided, both ceilings at the same height).
  -- Draws no wall, but unlike upper_open it DOES close the near ceiling:
  -- beyond this line the ceiling belongs to the back sector.
  SELECT seg_id, 'upper_flush', NULL, x_offset, y_offset, flags,
         f_ceil AS z_bot, b_ceil AS z_top,
         b_ceil AS v_anchor_base,FALSE AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND b_ceil = f_ceil
  AND (NOT (f_ceil_tex <=> 'F_SKY1')
       OR NOT (b_ceil_tex <=> 'F_SKY1'))

  UNION ALL
  -- LOWER step-up (two-sided, back floor higher than front floor)
  SELECT seg_id, 'lower', lower_tex, x_offset, y_offset, flags,
         f_floor AS z_bot, b_floor AS z_top,
         CASE WHEN (flags & 16)<>0 THEN f_ceil ELSE b_floor END AS v_anchor_base,
         FALSE AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND b_floor > f_floor

  UNION ALL
  -- FLOOR OPEN-DOWN (two-sided, back floor at or below the front floor). The
  -- riser faces the opposite side, so this camera-facing copy is clip-only.
  SELECT seg_id, 'lower_down', NULL, x_offset, y_offset, flags,
         b_floor AS z_bot, f_floor AS z_top,
         f_floor AS v_anchor_base,FALSE AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND b_floor <= f_floor

  UNION ALL
  -- MID MASKED (two-sided decorative; draws over the opening)
  SELECT seg_id, 'midmask', mid_tex, x_offset, y_offset, flags,
         /* pegging will decide exact vertical placement; provisional full height */
         f_floor AS z_bot, f_ceil AS z_top,
         CASE WHEN (flags & 16)<>0 THEN GREATEST(f_floor,b_floor)
              ELSE LEAST(f_ceil,b_ceil) END AS v_anchor_base,
         (flags & 16)<>0 AS v_anchor_add_tex_h,
         f_light, fsec, invx1, invx2, screen_x1, screen_x2, cx1, cx2, cy1, cy2, seg_u1, seg_u2
  FROM heights
  WHERE bsec IS NOT NULL
  AND mid_tex IS NOT NULL
  AND mid_tex <> '-' -- TODO
  AND f_ceil > f_floor            -- skip zero/inverted-height (dummy) sectors
),
wall_parts_tex AS (
  -- The texture is a property of the seg part, so it is looked up here, once
  -- per visible seg part (a few hundred rows).
  SELECT wp.*,
         COALESCE(m.width, 64)  AS tex_w,
         COALESCE(m.height, 64) AS tex_h,
         m.width                AS tex_width,
         m.tex_id
  FROM wall_parts wp
  LEFT JOIN walltex_meta m ON m.name = wp.tex
),
columns AS (
  SELECT
    w.*,
    sb.bsp_seq,
    (w.x_offset + w.seg_u1) AS u1,   -- wall U at the two endpoints
    (w.x_offset + w.seg_u2) AS u2,
    x AS col_x,
    x AS screen_x_clamped,           -- already clamped by bounds below
    -- clamp t to [0,1]: the generate_series range is FLOOR/CEIL of the
    -- projected span, which can extend one column past each endpoint at
    -- T-junctions. Without clamping, t leaves [0,1] and extrapolates the
    -- wall across the crack, producing 1px vertical strips of the wrong wall.
    LEAST(1.0, GREATEST(0.0,
      (x - screen_x1) / NULLIF(screen_x2 - screen_x1, 1e-6))) AS t
  FROM (
    SELECT w0.*,
      -- quick reject if the entire projected span is off-screen
      NOT (GREATEST(w0.screen_x1, w0.screen_x2) < 0 OR LEAST(w0.screen_x1, w0.screen_x2) >= rs0.screen_w) AS on_screen,
      GREATEST(0, CAST(FLOOR(LEAST(w0.screen_x1, w0.screen_x2)) AS SIGNED))    AS x_lo,
      LEAST  (rs0.screen_w-1, CAST(CEIL (GREATEST(w0.screen_x1, w0.screen_x2)) AS SIGNED)) AS x_hi
    FROM wall_parts_tex w0 CROSS JOIN render_settings rs0
  ) w
  CROSS JOIN render_settings rs
  JOIN seg_bsp sb ON sb.seg_id = w.seg_id
  CROSS JOIN LATERAL
    generate_series(w.x_lo, w.x_hi) AS x
  LEFT JOIN min_occlusion mo ON mo.col_x = x
  WHERE w.on_screen
    AND w.screen_x1 <> w.screen_x2
    -- occlusion: skip columns where a nearer (smaller bsp_seq) fully-solid
    -- wall has already closed off anything farther
    AND sb.bsp_seq <= COALESCE(mo.min_bsp_seq, sb.bsp_seq)
),
per_column AS (
  SELECT
    iv.*,
    1e0 / iv.invx                       AS depth_x,
    iv.u_over_x / NULLIF(iv.invx, 1e-6) AS u_col,
    -- scale factor at this column
    rs.focal * iv.invx                  AS scale
  FROM (
    SELECT c.*,
      -- inverse depth and perspective-correct U, interpolated per column
      c.invx1 + c.t * (c.invx2 - c.invx1)                       AS invx,
      (c.u1 * c.invx1) + c.t * ((c.u2 * c.invx2) - (c.u1 * c.invx1)) AS u_over_x
    FROM columns c
  ) iv
  CROSS JOIN render_settings rs
),
vertical AS (
  SELECT
    p.*,
    c.pz AS view_z,rs.cy AS screen_cy,
    (rs.cy - p.scale * (p.z_top - c.pz)) AS y_top_f,
    (rs.cy - p.scale * (p.z_bot - c.pz)) AS y_bot_f,
    -- tex_w / tex_h (texture size, 64x64 if missing) come with the seg part
    p.v_anchor_base
      + CASE WHEN p.v_anchor_add_tex_h THEN p.tex_h ELSE 0 END
      AS v_anchor_z
  FROM per_column p
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
),
clamped_spans AS (
  SELECT
    t.*,
    -- Doom samples wall V in world units. Inverting the perspective
    -- projection gives world_z at the first visible pixel; advancing one
    -- screen row changes V by 1/scale.
    t.v_anchor_z
      - (t.view_z+(t.screen_cy-t.y_start)/NULLIF(t.scale,1e-9))
      + t.y_offset AS v0,
    1.0/NULLIF(t.scale,1e-9) AS v_step
  FROM (
    SELECT
      v.*,
      -- clamp to [0 .. screen_h-1] in FLOAT, then cast
      CAST(CEIL (
        LEAST( rs.screen_h - 1.0,
               GREATEST(0.0, LEAST(v.y_top_f, v.y_bot_f))
        )
      ) AS SIGNED) AS y_start,
      CAST(FLOOR(
        LEAST( rs.screen_h - 1.0,
               GREATEST(0.0, GREATEST(v.y_top_f, v.y_bot_f))
        )
      ) AS SIGNED) AS y_end
    FROM vertical v
    CROSS JOIN render_settings rs
    -- keep only spans that intersect the screen vertically
    WHERE LEAST(v.y_top_f, v.y_bot_f) < rs.screen_h
      AND GREATEST(v.y_top_f, v.y_bot_f) >= 0
  ) t
  -- final guard: ignore empty/inverted spans
  WHERE t.y_end >= t.y_start
),
clamped_spans_lit AS (
  SELECT c.*,
         CASE WHEN wr.invuln THEN rs.light_index_invuln
              ELSE doom_light_index(c.f_light, b.light_bias + wr.extra_light,
                                    doom_light_scale(c.depth_x))
         END AS light_index,
         CAST((CAST(c.seg_id AS SIGNED)*8 + CASE c.part
            WHEN 'solid' THEN 0 WHEN 'upper' THEN 1 WHEN 'lower' THEN 2
            WHEN 'lower_down' THEN 3 ELSE 7 END) AS SIGNED) AS stable_id
  FROM clamped_spans c
  JOIN seg_light_bias b ON b.seg_id = c.seg_id
  CROSS JOIN weapon_runtime wr
  CROSS JOIN render_settings rs
),
fragments AS (
  SELECT
    c.seg_id, c.part, c.tex, c.tex_id, c.tex_width, c.light_index, c.stable_id,
    c.screen_x_clamped AS x,
    y AS y,
    c.depth_x    AS depth,
    -- integerized, positive-modulo UV (clean for the texture-table join)
    (CAST(FLOOR(c.u_col - c.tex_w * FLOOR(c.u_col / c.tex_w)) AS SIGNED)) AS u_i,
    ((CAST(FLOOR(c.v0 + (y - c.y_start)*c.v_step) AS SIGNED) % c.tex_h + c.tex_h) % c.tex_h) AS v_i,
    c.f_light AS sector_light,
    c.fsec     AS sector_id
  FROM clamped_spans_lit c
  CROSS JOIN LATERAL generate_series(c.y_start, c.y_end) AS y
  WHERE c.tex IS NOT NULL AND c.tex <> '-'
),
wall_tex AS (
  -- Sample the original PLAYPAL index; COLORMAP applies the actual Doom shade. A texture the WAD does not have,
  -- or a texel outside it, is index 0, as SQLDoom's bounds checks make it.
  SELECT
    f.x, f.y, f.depth, f.light_index, f.stable_id,
    COALESCE(tt.palette_index, 0) AS palette_index
  FROM fragments f
  LEFT JOIN walltex_texels tt ON tt.k = f.tex_id * 1048576 + f.v_i * 1024 + f.u_i
),
colored AS (
  SELECT w.x, w.y, w.depth, w.light_index, w.palette_index, w.stable_id
  FROM wall_tex w
),

-- VISPLANES!
-- You thought it was complicated before? This is where the fun starts!
-- Visplanes are a very elegant concept in a 2.5D game that is written in an
-- imperative language like C.
-- They are absolut hell in SQL.
--
-- Walls are correctly rendered now. We're missing floors and ceilings:
-- Everything that is not a wall is either a floor or a ceiling (called visplane in Doom).
-- This is a pretty ingenous hack to get 3D-y looking frames without having to
-- think about perspective - Doom just uses a pretty simple and fast floodfill algorithm.
--
-- We have to be more careful here and abuse window functions:
-- We go front-to-back per column. We maintain one open band [cc, fc] (ceiling-clip,
-- floor-clip) per column.
-- Each wall panel is either top-anchored (solid/upper: pushes cc down to its y_bot)
-- or bottom-anchored (solid/lower: pushes fc up to its y_top).
-- The pixel NOT covered by a wall, but inside the current open band,
-- is filled with the FRONT sector's ceiling (above) or floor (below). Through a
-- two-sided portal the upper+lower walls shrink the band from both sides.
-- Farther walls then fill that opening with the back sector's planes.
panel_cols AS (
  -- Drawable walls only matter when their vertical span intersects the screen.
  -- Unfortunately, we also have to look at portals completely off-screen:
  -- their projected front plane can still bound a visible floor or ceiling.
  SELECT
    v.col_x,
    v.seg_id,
    v.bsp_seq,
    v.part,
    v.depth_x,
    v.y_top_f,
    v.y_bot_f,
    -- Screen rows where this seg's front planes meet the boundary.
    CASE WHEN v.part = 'lower_down' THEN v.y_top_f ELSE v.y_bot_f END AS f_floor_y_f,
    CASE WHEN v.part = 'upper_open' THEN v.y_bot_f ELSE v.y_top_f END AS f_ceil_y_f,
    h.fsec,
    h.f_light,
    h.f_floor,
    h.f_ceil,
    (h.f_ceil_tex = 'F_SKY1') AS f_ceil_is_sky
  FROM vertical v
  JOIN heights h ON h.seg_id = v.seg_id
  CROSS JOIN render_settings rs
  WHERE v.part IN ('upper_open','upper_flush','lower_down')
     OR (LEAST(v.y_top_f, v.y_bot_f) < rs.screen_h
         AND GREATEST(v.y_top_f, v.y_bot_f) >= 0)
),
panel_seq AS (
  -- The front-to-back position of each panel within its column
  SELECT p.*,
         ROW_NUMBER() OVER (
           PARTITION BY p.col_x
           ORDER BY p.depth_x ASC, p.bsp_seq ASC, p.part, p.seg_id
         ) AS seq
  FROM panel_cols p
),
panel_clips AS (
  SELECT
    p.*,
    COALESCE(
      MAX(CASE
            WHEN p.part IN ('solid','upper')
              THEN CAST(FLOOR(LEAST(rs.screen_h - 1.0, GREATEST(0.0, p.y_bot_f))) AS SIGNED) + 1
            -- A flush boundary draws no wall.
            WHEN p.part = 'upper_flush'
              THEN GREATEST(0, CAST(FLOOR(LEAST(rs.screen_h - 1.0, p.y_bot_f)) AS SIGNED) + 1)
          END)
        OVER (PARTITION BY p.col_x ORDER BY p.depth_x ASC, p.bsp_seq ASC, p.part, p.seg_id
              ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING),
      0
    ) AS cc_before,
    -- A lower wall raises floorclip to the top of the riser (the bottom of the
    -- portal opening). A solid wall closes it in the same direction.
    COALESCE(
      MIN(CASE WHEN p.part IN ('solid','lower','lower_down')
               THEN LEAST(rs.screen_h - 1, CAST(CEIL(GREATEST(0.0, p.y_top_f)) AS SIGNED) - 1) END)
        OVER (PARTITION BY p.col_x ORDER BY p.depth_x ASC, p.bsp_seq ASC, p.part, p.seg_id
              ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING),
      rs.screen_h - 1
    ) AS fc_before,
    -- Doom starts a ceiling visplane only for a ceiling that is higher then the viewer.
    -- We need to continue drawing the ceiling that is already in porgress until we hit a wall.
    -- So let's carry it here.
    -- SQLDoom finds each with a top-1 LEFT JOIN LATERAL: the nearest earlier panel in the column whose ceiling is
    -- above the eye (whose floor is below it). That panel's seq is the largest such seq before this one, a window
    -- MAX, and its columns a join on it.
    pc.fsec AS carried_ceil_sec,
    pc.f_ceil AS carried_ceil_z,
    pc.f_light AS carried_ceil_light,
    COALESCE(pc.f_ceil_is_sky, FALSE) AS carried_ceil_is_sky,
    pf.fsec AS carried_floor_sec,
    pf.f_floor AS carried_floor_z,
    pf.f_light AS carried_floor_light
  FROM (
    SELECT q.*,
      MAX(CASE WHEN q.f_ceil > c0.pz THEN q.seq END)
        OVER (PARTITION BY q.col_x ORDER BY q.seq ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS ceil_seq,
      MAX(CASE WHEN q.f_floor < c0.pz THEN q.seq END)
        OVER (PARTITION BY q.col_x ORDER BY q.seq ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS floor_seq
    FROM panel_seq q CROSS JOIN cam c0
  ) p
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
  LEFT JOIN panel_seq pc ON pc.col_x = p.col_x AND pc.seq = p.ceil_seq
  LEFT JOIN panel_seq pf ON pf.col_x = p.col_x AND pf.seq = p.floor_seq
),
nearest_floor_panel AS (
  -- An upper wall changes only the ceiling and must not truncate the near
  -- floor. Bound it with the first solid/lower transition in each column.
  SELECT col_x, MIN(depth_x) AS depth_x
  FROM panel_cols
  WHERE part IN ('solid','lower','lower_down')
  GROUP BY col_x
),
nearest_ceiling_panel AS (
  -- Conversely, lower walls do not affect the ceiling. Bound the near ceiling
  -- with the first solid/upper transition.
  SELECT col_x, MIN(depth_x) AS depth_x
  FROM panel_cols
  WHERE part IN ('solid','upper','upper_open','upper_flush')
  GROUP BY col_x
),
plane_spans_raw AS (
  -- Ceiling fill up to the projected FRONT ceiling. upper_open is clip-only:
  -- it ends the near ceiling without drawing a wall or tightening cc.
  SELECT
    k.col_x,
    CASE WHEN k.f_ceil > c.pz OR k.f_ceil_is_sky
         THEN k.fsec    ELSE k.carried_ceil_sec   END AS sector_id,
    CASE WHEN k.f_ceil > c.pz OR k.f_ceil_is_sky
         THEN k.f_ceil  ELSE k.carried_ceil_z     END AS plane_z,
    CASE WHEN k.f_ceil > c.pz OR k.f_ceil_is_sky
         THEN k.f_light ELSE k.carried_ceil_light END AS sector_light,
    CASE WHEN k.f_ceil > c.pz OR k.f_ceil_is_sky
         THEN k.f_ceil_is_sky ELSE k.carried_ceil_is_sky END AS is_sky,
    'ceil' AS plane, 1 AS source_priority,
    GREATEST(0, k.cc_before) AS y0,
    k.y1
  FROM (
    SELECT k0.*, CAST(CEIL(LEAST(rs0.screen_h - 1.0, GREATEST(0.0,
      CASE
        -- The panel's own ceiling is bounded by the panel's own top edge.
        WHEN k0.f_ceil > c0.pz OR k0.f_ceil_is_sky THEN k0.f_ceil_y_f
        -- A carried ceiling has no wall of its own to end it, so it would run
        -- to the horizon and be sampled at thousands of map units per pixel.
        -- Stop it where sampling would alias.
        ELSE LEAST(k0.f_ceil_y_f, COALESCE(
                   rs0.cy - rs0.focal * (k0.carried_ceil_z - c0.pz)
                           / rs0.max_plane_depth, k0.f_ceil_y_f))
      END
    ))) AS SIGNED) - 1 AS y1
    FROM panel_clips k0 CROSS JOIN render_settings rs0 CROSS JOIN cam c0
  ) k
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
  WHERE k.part IN ('solid','upper','upper_open','upper_flush','midmask')
    AND k.y1 >= k.cc_before
    -- Sky is unbounded, so Doom fills any ceiling region that is not otherwise filled.
    AND (k.f_ceil > c.pz OR k.f_ceil_is_sky
         OR k.carried_ceil_is_sky OR k.cc_before > 0)
  UNION ALL
  -- A wall triggers a floor for it's "FRONT" sector. Starting from its floor boundary
  -- down to the floor clip established by nearer walls.
  -- This rule handles solid walls, step-up and step-down portals.
  -- We don't have to care abou the "BACK" sector, as farther, camera-facing
  -- sector sides should fill that later on.
  SELECT
    k.col_x,
    CASE WHEN k.f_floor < c.pz THEN k.fsec    ELSE k.carried_floor_sec   END AS sector_id,
    CASE WHEN k.f_floor < c.pz THEN k.f_floor ELSE k.carried_floor_z     END AS plane_z,
    CASE WHEN k.f_floor < c.pz THEN k.f_light ELSE k.carried_floor_light END AS sector_light,
    FALSE AS is_sky,
    'floor' AS plane, 1 AS source_priority,
    k.y0,
    LEAST(rs.screen_h - 1, k.fc_before) AS y1
  FROM (
    SELECT k0.*, CAST(FLOOR(LEAST(rs0.screen_h - 1.0, GREATEST(0.0,
      CASE
        WHEN k0.f_floor < c0.pz THEN k0.f_floor_y_f
        ELSE GREATEST(k0.f_floor_y_f, COALESCE(
                      rs0.cy - rs0.focal * (k0.carried_floor_z - c0.pz)
                              / rs0.max_plane_depth, k0.f_floor_y_f))
      END
    ))) AS SIGNED) + 1 AS y0
    FROM panel_clips k0 CROSS JOIN render_settings rs0 CROSS JOIN cam c0
  ) k
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
  WHERE k.part IN ('solid','midmask','lower','lower_down')
    AND k.y0 <= k.fc_before
    AND (k.f_floor < c.pz OR k.fc_before < rs.screen_h - 1)
  UNION ALL
  -- Seed the floor immediately around the camera.
  -- It continues until it reaches the nearest wall depth.
  SELECT
    x AS col_x,
    ps.id AS sector_id,
    ps.floor_height AS plane_z,
    ps.light_level AS sector_light,
    FALSE AS is_sky,
    'floor' AS plane,
    0 AS source_priority,
    CASE WHEN np.depth_x IS NULL THEN CAST(FLOOR(rs.cy) AS SIGNED) + 1
         ELSE CAST(FLOOR(LEAST(rs.screen_h - 1.0, GREATEST(0.0,
           rs.cy - rs.focal * (ps.floor_height - c.pz) / np.depth_x
         ))) AS SIGNED) + 1
    END AS y0,
    rs.screen_h - 1 AS y1
  FROM render_settings rs
  CROSS JOIN cam c
  CROSS JOIN player_sector ps
  CROSS JOIN LATERAL generate_series(0, rs.screen_w - 1) AS x
  LEFT JOIN nearest_floor_panel np ON np.col_x = x
  UNION ALL
  -- Same for the ceiling.
  SELECT
    x AS col_x,
    ps.id AS sector_id,
    ps.ceil_height AS plane_z,
    ps.light_level AS sector_light,
    (ps.ceil_tex = 'F_SKY1') AS is_sky,
    'ceil' AS plane,
    0 AS source_priority,
    0 AS y0,
    CASE WHEN np.depth_x IS NULL THEN CAST(CEIL(rs.cy) AS SIGNED) - 1
         ELSE CAST(CEIL(LEAST(rs.screen_h - 1.0, GREATEST(0.0,
           rs.cy - rs.focal * (ps.ceil_height - c.pz) / np.depth_x
         ))) AS SIGNED) - 1
    END AS y1
  FROM render_settings rs
  CROSS JOIN cam c
  CROSS JOIN player_sector ps
  CROSS JOIN LATERAL generate_series(0, rs.screen_w - 1) AS x
  LEFT JOIN nearest_ceiling_panel np ON np.col_x = x
),
plane_spans AS (
  -- A floor is only ever the top surface, visible from above (pz > plane_z);
  -- a ceiling is only ever the underside, visible from below (pz < plane_z).
  SELECT ps.*
  FROM plane_spans_raw ps
  CROSS JOIN cam c
  -- Vanilla's one exception (R_StoreWallRange): a ceiling below the viewer is
  -- skipped as "below view plane" ONLY when it is not the sky flat.
  WHERE (ps.plane = 'floor' AND ps.plane_z < c.pz)
     OR (ps.plane = 'ceil'  AND (ps.plane_z > c.pz OR ps.is_sky))
),
plane_spans_deduped AS (
  -- The near-camera seed and the nearest wall's front plane describe the same surface over the same rows.
  -- Collapse identical spans of one surface (same column, sector, height and side) before they are
  -- expanded: ~2k span rows instead of ~90k pixel rows.
  SELECT col_x, sector_id, plane_z, plane, is_sky, y0, y1,
         MIN(sector_light)    AS sector_light,
         MIN(source_priority) AS source_priority
  FROM plane_spans
  WHERE y1 >= y0
  GROUP BY col_x, sector_id, plane_z, plane, is_sky, y0, y1
),
plane_spans_prepared AS (
  -- Compute ray directions, sector texture and flat ID on the ~2k spans once rather than per pixel.
  SELECT
    ps.col_x, ps.sector_id, ps.plane_z, ps.plane, ps.y0, ps.y1,
    ps.sector_light, ps.source_priority,
    (CASE WHEN ps.plane = 'floor' THEN s.floor_tex ELSE s.ceil_tex END = 'F_SKY1') AS is_sky,
    fl.flat_id,
    rs.focal * (ps.plane_z - c.pz) AS plane_dz,
    c.px AS c_px,
    c.py AS c_py,
    (COS(c.view_rad) + ((ps.col_x - rs.cx) / rs.focal) * SIN(c.view_rad)) AS dir_x,
    (SIN(c.view_rad) - ((ps.col_x - rs.cx) / rs.focal) * COS(c.view_rad)) AS dir_y,
    ((CAST(FLOOR(degrees(c.view_rad - ATAN((ps.col_x - rs.cx) / rs.focal)) * (rs.sky_columns / rs.sky_degrees)) AS SIGNED)
      % CAST(rs.sky_columns AS SIGNED)) + CAST(rs.sky_columns AS SIGNED)) % CAST(rs.sky_columns AS SIGNED) AS sky_u
  FROM plane_spans_deduped ps
  CROSS JOIN cam c
  CROSS JOIN render_settings rs
  CROSS JOIN render_context rc
  JOIN sectors_lit s ON s.id = ps.sector_id AND s.map_id = rc.map_id
  LEFT JOIN flat_textures fl ON (CASE WHEN ps.plane = 'floor' THEN s.floor_tex ELSE s.ceil_tex END) <> 'F_SKY1'
                            AND fl.name = (CASE WHEN ps.plane = 'floor' THEN s.floor_tex ELSE s.ceil_tex END)
),
plane_fragments AS (
  SELECT
    sp.col_x AS x, y,
    CASE WHEN sp.is_sky THEN 1e7 ELSE sp.plane_dz / NULLIF(rs.cy - y, 0.0) END AS depth,
    sp.source_priority,
    CASE WHEN sp.is_sky THEN 0
         WHEN wr.invuln THEN rs.light_index_invuln
         ELSE doom_light_index(sp.sector_light, wr.extra_light,
                               doom_light_zdepth(CASE WHEN sp.is_sky THEN 1e7 ELSE sp.plane_dz / NULLIF(rs.cy - y, 0.0) END))
    END AS light_index,
    COALESCE(CASE WHEN sp.is_sky THEN wt.palette_index ELSE ft.palette_index END, 0) AS palette_index,
    CAST((CAST(sp.sector_id AS SIGNED)*2 + CASE WHEN sp.plane='floor' THEN 0 ELSE 1 END) AS SIGNED) AS stable_id
  FROM plane_spans_prepared sp
  CROSS JOIN render_settings rs
  CROSS JOIN weapon_runtime wr
  CROSS JOIN LATERAL generate_series(sp.y0, sp.y1) AS y
  CROSS JOIN (SELECT m.sky_texture AS name, wm.tex_id
              FROM maps m JOIN render_context rc ON rc.map_id = m.map_id
              LEFT JOIN walltex_meta wm ON wm.name = m.sky_texture) sk
  LEFT JOIN flat_texels ft
    ON NOT sp.is_sky AND ft.k = sp.flat_id * 1048576 + (((CAST(FLOOR(sp.c_py + (sp.plane_dz / NULLIF(rs.cy - y, 0.0)) * sp.dir_y) AS SIGNED) % rs.flat_size) + rs.flat_size) % rs.flat_size) * 1024 + (((CAST(FLOOR(sp.c_px + (sp.plane_dz / NULLIF(rs.cy - y, 0.0)) * sp.dir_x) AS SIGNED) % rs.flat_size) + rs.flat_size) % rs.flat_size)
  LEFT JOIN walltex_texels wt
    ON sp.is_sky AND wt.k = sk.tex_id * 1048576 + (((CAST(((y - rs.sky_horizon_y)) AS SIGNED) % rs.sky_rows) + rs.sky_rows) % rs.sky_rows) * 1024 + sp.sky_u
  WHERE sp.is_sky OR sp.plane_dz / NULLIF(rs.cy - y, 0.0) > rs.near
),

-- Next up: Sprites.
-- In Doom, they're just called things.
-- Each thing is just a 2D sprite that always faces you (so no real 3D object).
-- Their BSP-resolved sector and initial state sprite are pre-materialized at load time
-- in render_things
-- We only have to do view projection, select the right sprite variant (depending on the direction),
-- do sparse opaque-pixel sampling, and do the depth resolve.
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
thing_projected AS (
  -- The sprite frame for the rotation wanted, else the one drawn from every side (rotation 0): SQLDoom's top-1
  -- LATERAL, as the first row of each Thing's candidates.
  SELECT r.*
  FROM (
    SELECT
      tv.*, sf.lump_name, sl.lump_id, sf.flipped, sf.width, sf.height,
      sf.left_offset, sf.top_offset,
      rs.focal / tv.depth AS scale,
      rs.cx + rs.focal * tv.side / tv.depth AS origin_x,
      ROW_NUMBER() OVER (PARTITION BY tv.thing_id
                         ORDER BY CASE WHEN sf.rotation = tv.wanted_rotation THEN 0 ELSE 1 END) AS rot_rank
    FROM thing_view tv
    CROSS JOIN render_settings rs
    JOIN sprite_frames sf ON sf.sprite = tv.sprite AND sf.frame = tv.frame
      AND sf.rotation IN (0, tv.wanted_rotation)
    JOIN sprite_lumps sl ON sl.lump_name = sf.lump_name
    WHERE tv.depth >= 4.0
      -- Coarse reject leaves room for wide sprites whose origin is off-screen.
      AND ABS(tv.side) <= tv.depth * 2.0
  ) r
  WHERE r.rot_rank = 1
),
thing_bounds AS (
  SELECT
    tp.*,
    tp.origin_x - tp.scale * tp.left_offset AS x_left_f,
    tp.origin_x + tp.scale * (tp.width - tp.left_offset) AS x_right_f,
    rs.cy - tp.scale * (tp.base_z + tp.top_offset - c.pz) AS y_top_f,
    rs.cy - tp.scale
      * (tp.base_z + tp.top_offset - tp.height - c.pz) AS y_bottom_f
  FROM thing_projected tp
  CROSS JOIN render_settings rs
  CROSS JOIN cam c
),
thing_bounds_lit AS (
  -- A sprite's shade is per thing, not per pixel: one depth, one sector
  -- light. ~50 things instead of ~8,000 pixels through the light arithmetic.
  SELECT tb.*,
         CASE WHEN wr.invuln THEN rs.light_index_invuln
              WHEN tb.fullbright THEN 0
              ELSE doom_light_index(tb.sector_light, wr.extra_light,
                                    doom_light_scale(tb.depth))
         END AS base_light
  FROM thing_bounds tb
  CROSS JOIN weapon_runtime wr CROSS JOIN render_settings rs
),
thing_pixels AS (
  -- Each screen pixel a sprite's box covers, and the texel it samples: sprite_texels has only the opaque ones, so
  -- the join is SQLDoom's mask test.
  SELECT
    q.thing_id, q.sx AS x, q.sy AS y,
    q.depth, q.sector_light, q.fullbright, q.fuzz, q.base_light,
    -- R_InitTranslationTables: the PLAY sprite's green range (0x70..0x7F) is
    -- remapped per player: Slot 2 indigo (0x60), 3 brown (0x40), 4 red
    -- (0x20).
    CASE WHEN q.slot > 1 AND st.palette_index BETWEEN 112 AND 127
         THEN st.palette_index - 112 + CASE q.slot WHEN 2 THEN 96 WHEN 3 THEN 64 ELSE 32 END
         ELSE st.palette_index END AS palette_index
  FROM (
    SELECT tb.*, mpl.slot, screen_x AS sx, screen_y AS sy,
      LEAST(tb.width - 1, GREATEST(0,
        CAST(FLOOR((screen_x - tb.x_left_f) / tb.scale) AS SIGNED)
      )) AS raw_u,
      LEAST(tb.height - 1, GREATEST(0,
        CAST(FLOOR((screen_y - tb.y_top_f) / tb.scale) AS SIGNED)
      )) AS v_i
    FROM thing_bounds_lit tb
    CROSS JOIN render_settings rs
    CROSS JOIN render_context rc
    LEFT JOIN mp_players mpl ON mpl.map_id = rc.map_id AND mpl.player_thing_id = tb.thing_id
    CROSS JOIN LATERAL generate_series(
      GREATEST(0, CAST(FLOOR(tb.x_left_f) AS SIGNED)),
      LEAST(rs.screen_w - 1, CAST(CEIL(tb.x_right_f) AS SIGNED) - 1)
    ) AS screen_x
    CROSS JOIN LATERAL generate_series(
      GREATEST(0, CAST(FLOOR(tb.y_top_f) AS SIGNED)),
      LEAST(rs.screen_h - 1, CAST(CEIL(tb.y_bottom_f) AS SIGNED) - 1)
    ) AS screen_y
    WHERE tb.x_right_f > 0 AND tb.x_left_f < rs.screen_w
      AND tb.y_bottom_f > 0 AND tb.y_top_f < rs.screen_h
  ) q
  JOIN sprite_texels st ON st.k = q.lump_id * 1048576 + q.v_i * 1024
    + CASE WHEN q.flipped THEN q.width - 1 - q.raw_u ELSE q.raw_u END
),
sprite_fragments AS (
  SELECT
    sp.x,sp.y,sp.depth,sp.thing_id AS stable_id,sp.palette_index,
    -- Doom draws a Spectre with R_DrawFuzzColumn: the sprite's own colour is
    -- discarded and the pixel is re-read from the screen a row away through
    -- colormap 6, which looks like a shimmer. A set-based renderer resolves all
    -- pixels at once and has no finished screen to sample, so this instead
    -- keeps the silhouette and drives it to near-black with a per-pixel
    -- wobble.
    CASE WHEN sp.fuzz THEN
      CAST((26 + ((sp.x * 7 + sp.y * 13 + CAST(sp.depth AS SIGNED)) % 5)) AS SIGNED)
    ELSE sp.base_light
    END AS light_index
  FROM thing_pixels sp
),

fragment_union AS (
  SELECT x,y,depth,light_index,palette_index,
         2 AS surface_priority,0 AS source_priority,stable_id
  FROM colored
  UNION ALL
  SELECT x,y,depth,light_index,palette_index,
         1,0,stable_id
  FROM sprite_fragments
  UNION ALL
  SELECT x,y,depth,light_index,palette_index,
         0,CAST(source_priority AS SIGNED),stable_id
  FROM plane_fragments
),
ranked_fragments AS (
  -- The whole resolve key packed into one bigint, every field oriented so
  -- that smaller wins: depth first, then wall > sprite > plane, then the
  -- lower source_priority, then stable_id to keep exact ties from depending
  -- on sort input order.
  SELECT u.y*rs.screen_w+u.x AS pix,
         (CAST((GREATEST(0.0, LEAST(u.depth, 131071.0))*4096) AS SIGNED) << 34)
         | ((2 - u.surface_priority) << 32)
         | (LEAST(u.source_priority, 3) << 30)
         | (LEAST(65535, GREATEST(0, u.stable_id + 32768)) << 14)
         | (CAST(u.light_index AS SIGNED) << 8)
         | CAST(u.palette_index AS SIGNED) AS winner_key
  FROM fragment_union u CROSS JOIN render_settings rs
),
resolved AS (
  -- Nearest fragment per screen pixel, unpacked back out of the key.
  SELECT CAST((w.pix % rs.screen_w) AS SIGNED) AS x, CAST((w.pix DIV rs.screen_w) AS SIGNED) AS y,
         CAST(((w.winner_key >> 8) & 63) AS SIGNED) AS light_index,
         CAST((w.winner_key & 255) AS SIGNED) AS palette_index
  FROM (SELECT pix, MIN(winner_key) AS winner_key
        FROM ranked_fragments GROUP BY pix) w
  CROSS JOIN render_settings rs
),
-- Shotgun psprites
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
),
psprite_patches AS (
  SELECT p.*, sf.width, sf.height, sf.left_offset, sf.top_offset,
         sf.lump_name,
         p.sx - sf.left_offset AS x_left_f,
         p.sy - sf.top_offset + rs.cy - 100.0 AS y_top_f
  FROM psprite_layers p
  CROSS JOIN render_settings rs
  JOIN sprite_frames sf ON sf.sprite=p.sprite AND sf.frame=p.frame
    AND sf.rotation=0
  JOIN sprite_lumps sl ON sl.lump_name=sf.lump_name
),
psprite_pixels AS (
  -- The weapon is drawn 1:1, so each opaque texel is one screen pixel, placed from the patch's top-left corner.
  SELECT q.layer, q.x0 + st.u AS x, q.y0 + st.v AS y, q.fullbright, st.palette_index
  FROM (SELECT p.*, CAST(FLOOR(p.x_left_f) AS SIGNED) AS x0, CAST(FLOOR(p.y_top_f) AS SIGNED) AS y0
        FROM psprite_patches p) q
  JOIN sprite_texels st ON st.lump_name = q.lump_name
  CROSS JOIN render_settings rs
  WHERE q.x0 + st.u BETWEEN 0 AND rs.screen_w - 1
    AND q.y0 + st.v BETWEEN 0 AND rs.screen_h - 1
),
psprite_resolved AS (
  SELECT p.x,p.y,p.palette_index,
    CASE WHEN wr.invuln THEN rs.light_index_invuln
         WHEN p.fullbright THEN 0
         ELSE doom_light_index(ps.light_level, wr.extra_light,
                               rs.light_psprite_bias)
    END AS light_index
  FROM (
    SELECT pp.*, ROW_NUMBER() OVER (
      PARTITION BY pp.x,pp.y ORDER BY pp.layer DESC) AS rn
    FROM psprite_pixels pp
  ) p
  CROSS JOIN player_sector ps CROSS JOIN weapon_runtime wr
  CROSS JOIN render_settings rs
  WHERE p.rn=1
),
final_pixels AS (
  -- The weapon over the view (SQLDoom's FULL JOIN, which MySQL lacks): every resolved pixel, the weapon's where it
  -- has one, and the weapon's pixels that no fragment reached.
  SELECT r.x, r.y,
         COALESCE(p.light_index,r.light_index) AS light_index,
         COALESCE(p.palette_index,r.palette_index) AS palette_index
  FROM resolved r LEFT JOIN psprite_resolved p ON p.x = r.x AND p.y = r.y
  UNION ALL
  SELECT p.x, p.y, p.light_index, p.palette_index
  FROM psprite_resolved p
  WHERE NOT EXISTS (SELECT 1 FROM resolved r WHERE r.x = p.x AND r.y = p.y)
),

-- The original DOOM status bar
player_ui_state AS (
  SELECT ps.health, ps.alive, ps.pain_face_tics, ps.armor,
         ps.ammo_bullets, ps.ammo_shells, ps.ammo_rockets, ps.ammo_cells,
         ps.key_blue,ps.key_yellow,ps.key_red,
         ps.damage_count, ps.bonus_count, ps.radsuit_tics,
         ps.message, ps.message_tics, ps.backpack,
         wd.ammo_type AS current_ammo_type
  FROM player_state ps
  CROSS JOIN render_context rc
  LEFT JOIN player_weapons w ON w.map_id = ps.map_id
    AND w.player_thing_id = ps.player_thing_id
  LEFT JOIN weapon_defs wd ON wd.weapon_id = COALESCE(w.current_weapon, 2)
  WHERE ps.map_id = rc.map_id AND ps.player_thing_id = rc.player_thing_id
),
active_palette AS (
  -- ST_doPaletteStuff, verbatim:
  --   if (cnt)            palette = min((cnt+7)>>3, NUMREDPALS-1)   + STARTREDPALS
  --   else if (bonus)     palette = min((b+7)>>3, NUMBONUSPALS-1) + STARTBONUSPALS
  --   else if (ironfeet > 4*32 || ironfeet & 8)  palette = RADIATIONPAL
  --   else                palette = 0
  -- with STARTREDPALS 1, NUMREDPALS 8, STARTBONUSPALS 9, NUMBONUSPALS 4 and
  -- RADIATIONPAL 13. The & 8 is what makes the radiation suit's green blink
  -- as it runs out.
  SELECT CASE
    WHEN pu.damage_count > 0
      THEN LEAST(7, (pu.damage_count + 7) DIV 8) + 1
    WHEN pu.bonus_count > 0
      THEN LEAST(3, (pu.bonus_count + 7) DIV 8) + 9
    WHEN pu.radsuit_tics > 128 OR (pu.radsuit_tics & 8) <> 0
      THEN 13
    ELSE 0
  END AS pal
  FROM player_ui_state pu
),
palette_map AS (
  -- This frame's palette
  SELECT cm.level, cm.palette_index, cm.rgb
  FROM colormap_rgb cm
  CROSS JOIN active_palette ap
  WHERE cm.pal = ap.pal
),
view_colored AS (
  -- Resolve COLORMAP on the view's pixels.
  SELECT r.x, r.y, r.y * 4096 + r.x AS pix, cm.rgb
  FROM final_pixels r
  LEFT JOIN palette_map cm
    ON cm.level = r.light_index
   AND cm.palette_index = r.palette_index
),
view_holes AS (
  SELECT 0 AS x, 0 AS y, X'000000' AS rgb
  WHERE FALSE
)
-- @hud-cut
,
weapon_ownership AS (
  SELECT slot.weapon_id,
         EXISTS (
           SELECT 1 FROM player_weapon_owned o CROSS JOIN render_context rc
           WHERE o.map_id=rc.map_id AND o.player_thing_id=rc.player_thing_id
             AND o.weapon_id=slot.weapon_id
         ) AS owned
  FROM generate_series(2,7) slot(weapon_id)
),
weapon_layers AS (
  -- Vanilla's ARMS panel shows weapon slots 2..7: gray while unavailable,
  -- yellow once owned. Fist/chainsaw share slot 1 and are not listed here.
  SELECT 2 AS layer,
         CONCAT(CASE WHEN owned THEN 'STYSNUM' ELSE 'STGNUM' END,
                CAST(weapon_id AS CHAR)) AS patch,
         111+((weapon_id-2)%3)*12 AS x,
         172+((weapon_id-2) DIV 3)*10 AS y
  FROM weapon_ownership
),
ui_face AS (
  SELECT
    CASE
      WHEN NOT pu.alive THEN 'STFDEAD0'
      WHEN pu.pain_face_tics > 0 THEN
        CONCAT('STFOUCH', CAST(LEAST(4, GREATEST(0, (100 - GREATEST(0, LEAST(100, pu.health))) DIV 20)) AS CHAR))
      ELSE
        CONCAT('STFST', CAST(LEAST(4, GREATEST(0, (100 - GREATEST(0, LEAST(100, pu.health))) DIV 20)) AS CHAR), '0')
    END AS face_patch
  FROM player_ui_state pu
),
status_bar AS (
  SELECT
    rs.screen_h     AS top_y,
    rs.screen_h + 4 AS digit_y,
    90         AS health_right_x,
    221        AS armor_right_x,
    44         AS ammo_right_x,
    288        AS ammo_box_right_x,
    314        AS max_ammo_right_x,
    239        AS key_x,
    143        AS face_x,
    14         AS tall_digit_w,
    4          AS short_digit_w 
  FROM render_settings rs
),
health_digits AS (SELECT pu.health AS value, sb.health_right_x AS right_x
                  FROM player_ui_state pu CROSS JOIN status_bar sb),
armor_digits AS (SELECT pu.armor AS value, sb.armor_right_x AS right_x
                 FROM player_ui_state pu CROSS JOIN status_bar sb),
ammo_digits AS (
  SELECT
    CASE pu.current_ammo_type
      WHEN 'bullets' THEN pu.ammo_bullets WHEN 'shells' THEN pu.ammo_shells
      WHEN 'rockets' THEN pu.ammo_rockets WHEN 'cells' THEN pu.ammo_cells
    END AS value,
    sb.ammo_right_x AS right_x
  FROM player_ui_state pu CROSS JOIN status_bar sb
  WHERE pu.current_ammo_type IS NOT NULL
),
-- The small always-visible sub-box showing all 4 ammo types at once, independent of which
-- weapon is currently equipped.
ammo_type_values AS (
  SELECT pu.ammo_bullets AS value, sb.ammo_box_right_x AS right_x, 173 AS y
  FROM player_ui_state pu CROSS JOIN status_bar sb
  UNION ALL
  SELECT pu.ammo_shells, sb.ammo_box_right_x, 179
  FROM player_ui_state pu CROSS JOIN status_bar sb
  UNION ALL
  SELECT pu.ammo_rockets, sb.ammo_box_right_x, 185
  FROM player_ui_state pu CROSS JOIN status_bar sb
  UNION ALL
  SELECT pu.ammo_cells, sb.ammo_box_right_x, 191
  FROM player_ui_state pu CROSS JOIN status_bar sb
),
ammo_max_values AS (
  SELECT (CASE WHEN pu.backpack THEN ad.backpack_cap ELSE ad.cap END) AS value,
         sb.max_ammo_right_x AS right_x,
         (CASE ad.ammo_type WHEN 'bullets' THEN 173 WHEN 'shells' THEN 179
                            WHEN 'rockets' THEN 185 ELSE 191 END) AS y
  FROM player_ui_state pu CROSS JOIN status_bar sb
  CROSS JOIN ammo_defs ad
),
key_layers AS (
  SELECT 1 AS layer,'STKEYS0' AS patch,sb.key_x AS x,171 AS y
  FROM player_ui_state CROSS JOIN status_bar sb WHERE key_blue
  UNION ALL SELECT 1,'STKEYS1',sb.key_x,181
  FROM player_ui_state CROSS JOIN status_bar sb WHERE key_yellow
  UNION ALL SELECT 1,'STKEYS2',sb.key_x,191
  FROM player_ui_state CROSS JOIN status_bar sb WHERE key_red
),
digit_layers AS (
  -- Digits are right-aligned ending at right_x, one row of generate_series
  -- per character position
  SELECT 1 AS layer,
    CONCAT('STTNUM', substring(CAST(hd.value AS CHAR) FROM gs.i FOR 1)) AS patch,
    hd.right_x - (length(CAST(hd.value AS CHAR)) - gs.i + 1) * sb.tall_digit_w AS x,
    sb.digit_y AS y
  FROM health_digits hd CROSS JOIN status_bar sb
  CROSS JOIN LATERAL generate_series(1, length(CAST(hd.value AS CHAR))) AS gs(i)
  UNION ALL
  SELECT 1, CONCAT('STTNUM', substring(CAST(ad.value AS CHAR) FROM gs.i FOR 1)) AS patch,
    ad.right_x - (length(CAST(ad.value AS CHAR)) - gs.i + 1) * sb.tall_digit_w AS x,
    sb.digit_y AS y
  FROM armor_digits ad CROSS JOIN status_bar sb
  CROSS JOIN LATERAL generate_series(1, length(CAST(ad.value AS CHAR))) AS gs(i)
  UNION ALL
  SELECT 1, CONCAT('STTNUM', substring(CAST(am.value AS CHAR) FROM gs.i FOR 1)) AS patch,
    am.right_x - (length(CAST(am.value AS CHAR)) - gs.i + 1) * sb.tall_digit_w AS x,
    sb.digit_y AS y
  FROM ammo_digits am CROSS JOIN status_bar sb
  CROSS JOIN LATERAL generate_series(1, length(CAST(am.value AS CHAR))) AS gs(i)
  UNION ALL
  SELECT 1, CONCAT('STYSNUM', substring(CAST(at.value AS CHAR) FROM gs.i FOR 1)) AS patch,
    at.right_x - (length(CAST(at.value AS CHAR)) - gs.i + 1) * sb.short_digit_w AS x,
    at.y
  FROM ammo_type_values at CROSS JOIN status_bar sb
  CROSS JOIN LATERAL generate_series(1, length(CAST(at.value AS CHAR))) AS gs(i)
  UNION ALL
  SELECT 1, CONCAT('STYSNUM', substring(CAST(mx.value AS CHAR) FROM gs.i FOR 1)) AS patch,
    mx.right_x - (length(CAST(mx.value AS CHAR)) - gs.i + 1) * sb.short_digit_w AS x,
    mx.y
  FROM ammo_max_values mx CROSS JOIN status_bar sb
  CROSS JOIN LATERAL generate_series(1, length(CAST(mx.value AS CHAR))) AS gs(i)
),
ui_layers AS (
  SELECT layer, patch, x, y FROM digit_layers
  UNION ALL SELECT layer,patch,x,y FROM key_layers
  UNION ALL SELECT layer,patch,x,y FROM weapon_layers
  UNION ALL SELECT 1, face_patch, sb.face_x, sb.top_y
  FROM ui_face CROSS JOIN status_bar sb
),
ui_layer_patches AS (
  SELECT l.layer,l.patch,l.x-p.left_offset AS dest_x,
         l.y-p.top_offset AS dest_y
  FROM ui_layers l
  JOIN ui_patches p ON p.name = l.patch
),
ui_pixels AS (
  SELECT ulp.layer, (ulp.dest_y+px.dy) * 4096 + (ulp.dest_x+px.dx) AS pix,
         px.palette_index
  FROM ui_layer_patches ulp
  JOIN ui_hud_pixels px ON px.name=ulp.patch
),
ui_resolved AS (
  -- The top layer's index at each pixel (SQLDoom's ARG_MAX(palette_index, layer)).
  SELECT pix, palette_index
  FROM (SELECT p.layer, p.pix, p.palette_index, ROW_NUMBER() OVER (PARTITION BY p.pix ORDER BY p.layer DESC) AS rn FROM ui_pixels p) r
  WHERE rn = 1
),
ui_colored AS (
  SELECT s.y * 4096 + s.x AS pix,
         COALESCE(ovcm.rgb,
                  CASE WHEN ap.pal = 0 THEN s.rgb ELSE barcm.rgb END,
                  s.rgb) AS rgb
  FROM ui_static_pixels s
  CROSS JOIN active_palette ap
  LEFT JOIN ui_resolved ur ON ur.pix = s.y * 4096 + s.x
  LEFT JOIN palette_map ovcm
    ON ovcm.level = 0 AND ovcm.palette_index = ur.palette_index
  LEFT JOIN palette_map barcm
    ON ap.pal <> 0 AND barcm.level = 0
   AND barcm.palette_index = s.palette_index
)
SELECT CONCAT(
  (SELECT GROUP_CONCAT(rgb ORDER BY pix SEPARATOR '') FROM view_colored),
  (SELECT GROUP_CONCAT(rgb ORDER BY pix SEPARATOR '') FROM ui_colored)
) AS frame_rgb;
