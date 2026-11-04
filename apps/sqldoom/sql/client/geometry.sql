-- The 3D renderer's geometry, after sql/client/scene_common.sql: one tic's walls, floors and ceilings as triangles
-- for r3d_draw_tex (play.gr). SQLDoom's map, sectors and effects are read as the game tic leaves them; the renderer
-- is no longer the 320x200 software one (sql/renderer.sql) but a vertex stage: surfaces placed, culled and lit
-- here, and filled at the window's own size in true colour by the native rasterizer.
--
-- One row a triangle: x0 y0 z0 u0 v0 .. x2 y2 z2 u2 v2 (points on the map: x east, y north, z up; u v in texels),
-- tex (a texture id play.gr gave r3d_texture: walls their tex_id, flats 10000 + flat_id), light (0..255) and flags
-- (1 masked, 4 clamp v).
--
-- Doom's rules kept: a seg is drawn from its front only; a two-sided line has a lower wall where the floor behind
-- is higher, an upper wall where the ceiling behind is lower (none between two skies) and a masked middle that
-- is not repeated up or down; upper and lower unpegging; walls along the x axis a light step darker, along y one
-- brighter; flats repeat every 64 units on the map's grid; a sky ceiling is not drawn (the sky is behind it).
--
-- It is worked out once a game tic, and each frame until the next turns the same points to the camera where it is
-- then (r3d_draw_tex's camera): so what is culled is culled with room to spare (a wall or a floor is dropped only
-- when it is well behind the eye).
,
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
quad_half (h) AS (SELECT 0 UNION ALL SELECT 1)
-- A wall quad is two triangles: (A B C) and (A C D) of its corners A (start, bottom), B (end, bottom),
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
