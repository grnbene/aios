-- Pick Rush: one frame's triangles, in camera space, for r3d_draw. The vertex half of the renderer, as one query.
--
-- Reads @cx @cy @cz (the camera), @cyaw (its heading; 0 looks along +z) and @cpitch (radians, down positive),
-- @tanh (the tangent of half the horizontal field of view) and @far (metres). Returns one row a triangle:
-- x0 y0 z0 x1 y1 z1 x2 y2 z2 (camera space: x right, y up, z forward), col (0xRRGGBB, lit and fogged) and em (which
-- edges are box edges).
--
-- Every part is placed in the world (its entity's position and turn, its forks' height), kept when its bounding
-- sphere is ahead of the camera and inside the view's sides, and broken into the faces that look at the camera:
-- a face of a box looks at a point when the point is beyond the face's plane, n . (point - centre) > half the
-- box's size along n. A face is lit by one light from above and fogged by its distance.
WITH
cam AS (
  SELECT @cx AS cx, @cy AS cy, @cz AS cz,
         COS(@cyaw) AS yc, SIN(@cyaw) AS ys, COS(@cpitch) AS pc, SIN(@cpitch) AS ps,
         @tanh AS tanh, @far AS far
),
placed AS (
  SELECT pt.part_id, pt.sx, pt.sy, pt.sz, pt.color, pt.rad,
         COS(e.yaw) AS ec, SIN(e.yaw) AS es,
         e.x + pt.ox * COS(e.yaw) + pt.oz * SIN(e.yaw) AS wx,
         e.y + pt.oy + pt.lift * e.fork_h AS wy,
         e.z - pt.ox * SIN(e.yaw) + pt.oz * COS(e.yaw) AS wz
  FROM parts pt JOIN entities e ON e.entity_id = pt.entity_id
),
seen AS (
  SELECT p.*,
         SQRT(POW(p.wx - c.cx, 2) + POW(p.wy - c.cy, 2) + POW(p.wz - c.cz, 2)) AS dist
  FROM placed p CROSS JOIN cam c
  WHERE (p.wx - c.cx) * c.ys + (p.wz - c.cz) * c.yc > -p.rad - 1
    AND (p.wx - c.cx) * c.ys + (p.wz - c.cz) * c.yc < c.far + p.rad
    AND ABS((p.wx - c.cx) * c.yc - (p.wz - c.cz) * c.ys)
        < ((p.wx - c.cx) * c.ys + (p.wz - c.cz) * c.yc) * c.tanh + p.rad * 1.5 + 0.5
),
faces AS (
  SELECT s.part_id, f.f,
         0.45 + 0.55 * GREATEST(0, (f.nx * s.ec + f.nz * s.es) * 0.3 + f.ny * 0.86 + (f.nz * s.ec - f.nx * s.es) * 0.41)
           + 0.08 * (f.nx * s.ec + f.nz * s.es) AS lit,
         LEAST(1, GREATEST(0, (s.dist - 22) / 40)) AS fog,
         s.color
  FROM seen s CROSS JOIN box_faces f CROSS JOIN cam c
  WHERE (f.nx * s.ec + f.nz * s.es) * (c.cx - s.wx) + f.ny * (c.cy - s.wy) + (f.nz * s.ec - f.nx * s.es) * (c.cz - s.wz)
        > ABS(f.nx) * s.sx / 2 + ABS(f.ny) * s.sy / 2 + ABS(f.nz) * s.sz / 2
),
corners AS (
  SELECT q.part_id, v.v,
         (q.wx + v.x * q.sx * q.ec + v.z * q.sz * q.es - c.cx) * c.yc
           - (q.wz - v.x * q.sx * q.es + v.z * q.sz * q.ec - c.cz) * c.ys AS rx,
         q.wy + v.y * q.sy - c.cy AS ry,
         (q.wx + v.x * q.sx * q.ec + v.z * q.sz * q.es - c.cx) * c.ys
           + (q.wz - v.x * q.sx * q.es + v.z * q.sz * q.ec - c.cz) * c.yc AS rz
  FROM seen q CROSS JOIN box_verts v CROSS JOIN cam c
),
cam_corners AS (
  SELECT k.part_id, k.v, k.rx AS x, k.ry * c.pc + k.rz * c.ps AS y, k.rz * c.pc - k.ry * c.ps AS z
  FROM corners k CROSS JOIN cam c
)
SELECT a.x AS x0, a.y AS y0, a.z AS z0, b.x AS x1, b.y AS y1, b.z AS z1, d.x AS x2, d.y AS y2, d.z AS z2,
       (ROUND((((fa.color >> 16) & 255) * LEAST(1, fa.lit)) * (1 - fa.fog) + 28 * fa.fog) << 16)
     | (ROUND((((fa.color >> 8) & 255) * LEAST(1, fa.lit)) * (1 - fa.fog) + 32 * fa.fog) << 8)
     |  ROUND(((fa.color & 255) * LEAST(1, fa.lit)) * (1 - fa.fog) + 40 * fa.fog) AS col,
       t.em
FROM faces fa
JOIN box_tris t ON t.f = fa.f
JOIN cam_corners a ON a.part_id = fa.part_id AND a.v = t.a
JOIN cam_corners b ON b.part_id = fa.part_id AND b.v = t.b
JOIN cam_corners d ON d.part_id = fa.part_id AND d.v = t.c
