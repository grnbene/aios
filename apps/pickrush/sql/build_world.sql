-- Pick Rush: the warehouse, built from nothing but the tables in schema.sql. A statement ends at `;;`.
--
-- Floor plan (metres; x east, z north, y up):
--   x 0..44, z 0..36, walls all round.
--   Six rack rows, A..F, along x from 6 to 28.4: row r stands at z = 6 + 4.8 r, 1.2 deep, eight 2.8 m bays, four
--   levels (floor, 1.3, 2.6, 3.9). Pallets go in from the south side (a forklift facing north, yaw 0).
--   Aisles 3.6 wide south of each row; cross aisles at x 3 and x 31.5.
--   Receiving dock x 34..42, z 2..10; shipping dock x 34..42, z 26..34.

-- A deterministic draw in [0, 1) from two integers: the same world and the same tic draw the same numbers. The
-- scale is a DOUBLE literal: dividing by 4294967296.0 would be MySQL's decimal division, four places and no more.
DROP FUNCTION IF EXISTS pr_rand;;
-- Park-Miller steps (x * 48271 mod 2^31 - 1) with an xor-shift between them, so that neighbouring inputs (slot ids,
-- tics) do not land on a lattice; every product stays well inside a BIGINT, whatever the inputs (a CRC32 is up to
-- 2^32).
CREATE FUNCTION pr_rand(a BIGINT, b BIGINT) RETURNS DOUBLE DETERMINISTIC
BEGIN
  DECLARE h BIGINT;
  SET h = (ABS(a) % 2147483647 + 1) * 48271 % 2147483647;
  SET h = ((h ^ (h DIV 65536)) % 2147483647 + (ABS(b) % 2147483647) * 16807 % 2147483647) % 2147483647;
  SET h = ((h ^ (h DIV 8192)) % 2147483647 + 1) * 48271 % 2147483647;
  SET h = ((h ^ (h DIV 131072)) % 2147483647 + 1) * 48271 % 2147483647;
  RETURN h * 4.656612875245797e-10;
END;;

-- The unit box: corner v has x = bit 0, y = bit 1, z = bit 2, each less a half.
INSERT INTO box_verts (v, x, y, z)
WITH RECURSIVE n (v) AS (SELECT 0 UNION ALL SELECT v + 1 FROM n WHERE v < 7)
SELECT v, (v & 1) - 0.5, ((v >> 1) & 1) - 0.5, ((v >> 2) & 1) - 0.5 FROM n;;

INSERT INTO box_faces (f, nx, ny, nz) VALUES
  (0, 1, 0, 0), (1, -1, 0, 0), (2, 0, 1, 0), (3, 0, -1, 0), (4, 0, 0, 1), (5, 0, 0, -1);;

-- Each face is the quad p0 p1 p2 p3 (in order round it), split into (p0 p1 p2) and (p0 p2 p3): the first's box
-- edges are p0-p1 and p1-p2 (em 3), the second's p2-p3 and p3-p0 (em 6).
INSERT INTO box_tris (t, f, a, b, c, em) VALUES
  (0, 0, 1, 3, 7, 3), (1, 0, 1, 7, 5, 6),
  (2, 1, 0, 2, 6, 3), (3, 1, 0, 6, 4, 6),
  (4, 2, 2, 3, 7, 3), (5, 2, 2, 7, 6, 6),
  (6, 3, 0, 1, 5, 3), (7, 3, 0, 5, 4, 6),
  (8, 4, 4, 5, 7, 3), (9, 4, 4, 7, 6, 6),
  (10, 5, 0, 1, 3, 3), (11, 5, 0, 3, 2, 6);;

INSERT INTO skus (sku, name, color) VALUES
  ('TIR-205', 'Tires 205/55R16', 0x3a3a40),
  ('OIL-5W30', 'Motor oil 5W-30', 0xd9a520),
  ('BAT-H6', 'Batteries H6', 0x2f6fd8),
  ('FLT-AIR', 'Air filters', 0xe8e8e0),
  ('WIP-22', 'Wiper blades 22in', 0x30a050),
  ('BRK-PAD', 'Brake pads', 0xc03030),
  ('COOL-50', 'Coolant 50/50', 0x40c8c0),
  ('PLG-IR', 'Spark plugs', 0x9050c0);;

-- Static entities sit at the origin unturned, so a static part's offset is its place in the world.
INSERT INTO entities (entity_id, kind) VALUES (10, 'static'), (11, 'static'), (12, 'static'), (13, 'static'),
  (14, 'static'), (15, 'static'), (16, 'static');;

-- The building: floor, walls, dock pads, dock doors.
INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color, solid) VALUES
  (10, 22, -0.1, 18, 46, 0.2, 38, 0, 0x7d8288, 0),
  (10, 22, 3.5, -0.15, 44.6, 7, 0.3, 0, 0xa9b0b8, 1),
  (10, 22, 3.5, 36.15, 44.6, 7, 0.3, 0, 0xa9b0b8, 1),
  (10, -0.15, 3.5, 18, 0.3, 7, 36, 0, 0xa9b0b8, 1),
  (10, 44.15, 3.5, 18, 0.3, 7, 36, 0, 0xa9b0b8, 1),
  (10, 38, 0.005, 6, 8, 0.01, 8, 0, 0x2b6cb0, 0),
  (10, 38, 0.005, 30, 8, 0.01, 8, 0, 0x2f9e44, 0),
  (10, 43.98, 1.8, 6, 0.06, 3.6, 4, 0, 0x30343a, 0),
  (10, 43.98, 1.8, 30, 0.06, 3.6, 4, 0, 0x30343a, 0),
  (10, 43.95, 3.85, 6, 0.1, 0.3, 4.4, 0, 0xe0b020, 0),
  (10, 43.95, 3.85, 30, 0.1, 0.3, 4.4, 0, 0xe0b020, 0);;

-- Yellow floor lines down both sides of each aisle.
INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color, solid)
WITH RECURSIVE r (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 5)
SELECT 10, 17.2, 0.006, 6 + 4.8 * n - 0.75, 22.6, 0.01, 0.1, 0, 0xe0b020, 0 FROM r;;

-- Rack uprights: a post at the front and back of each of the nine frames of every row.
INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color, solid)
WITH RECURSIVE r (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 5),
     b (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM b WHERE n < 8),
     side (s) AS (SELECT -1 UNION ALL SELECT 1)
SELECT 11 + r.n, 6 + 2.8 * b.n, 2.3, 6 + 4.8 * r.n + 0.55 * side.s, 0.1, 4.6, 0.1, 0, 0x2a5db0, 1
FROM r CROSS JOIN b CROSS JOIN side;;

-- Decks at the three upper levels of every bay (the floor is level 1).
INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color, solid)
WITH RECURSIVE r (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 5),
     b (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM b WHERE n < 7),
     l (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM l WHERE n < 3)
SELECT 11 + r.n, 6 + 2.8 * b.n + 1.4, 1.3 * l.n - 0.06, 6 + 4.8 * r.n, 2.7, 0.12, 1.2, 0, 0xe06a1a, 1
FROM r CROSS JOIN b CROSS JOIN l;;

-- The bin locations.
INSERT INTO slots (slot_id, label, rack_row, bay, level, x, y, z)
WITH RECURSIVE r (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 5),
     b (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM b WHERE n < 7),
     l (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM l WHERE n < 3)
SELECT r.n * 100 + b.n * 10 + l.n,
       CONCAT(CHAR(65 + r.n), '-', LPAD(b.n + 1, 2, '0'), '-', l.n + 1),
       r.n, b.n, l.n, 6 + 2.8 * b.n + 1.4, 1.3 * l.n, 6 + 4.8 * r.n
FROM r CROSS JOIN b CROSS JOIN l;;

-- Stock: about six slots in ten hold a pallet.
INSERT INTO entities (entity_id, kind, x, y, z, yaw, sku, slot_id)
SELECT 1000 + s.slot_id, 'pallet', s.x, s.y, s.z, 0,
       ELT(1 + FLOOR(pr_rand(s.slot_id, 11) * 8),
           'TIR-205', 'OIL-5W30', 'BAT-H6', 'FLT-AIR', 'WIP-22', 'BRK-PAD', 'COOL-50', 'PLG-IR'),
       s.slot_id
FROM slots s WHERE pr_rand(s.slot_id, 7) < 0.6;;

UPDATE slots s JOIN entities e ON e.slot_id = s.slot_id AND e.kind = 'pallet' SET s.pallet_id = e.entity_id;;

-- The player's forklift (1) and the other three (2-4), from one set of parts; the body is painted per truck.
DROP TABLE IF EXISTS forklift_model;;
CREATE TABLE forklift_model (
  n INT PRIMARY KEY,
  ox DOUBLE, oy DOUBLE, oz DOUBLE, sx DOUBLE, sy DOUBLE, sz DOUBLE,
  color INT, painted INT, lift INT
);;
INSERT INTO forklift_model VALUES
  (1, 0, 0.55, -0.25, 1.2, 0.7, 1.9, 0, 1, 0),
  (2, 0, 0.75, -1.05, 1.2, 0.8, 0.45, 0x383c44, 0, 0),
  (3, 0, 2.15, -0.35, 1.2, 0.08, 1.3, 0x383c44, 0, 0),
  (4, -0.56, 1.5, 0.25, 0.08, 1.25, 0.08, 0x383c44, 0, 0),
  (5, 0.56, 1.5, 0.25, 0.08, 1.25, 0.08, 0x383c44, 0, 0),
  (6, -0.56, 1.5, -0.95, 0.08, 1.25, 0.08, 0x383c44, 0, 0),
  (7, 0.56, 1.5, -0.95, 0.08, 1.25, 0.08, 0x383c44, 0, 0),
  (8, 0, 1.1, -0.45, 0.6, 0.45, 0.5, 0x202020, 0, 0),
  (9, -0.42, 1.75, 0.8, 0.1, 3.5, 0.12, 0x5a5e66, 0, 0),
  (10, 0.42, 1.75, 0.8, 0.1, 3.5, 0.12, 0x5a5e66, 0, 0),
  (11, 0, 0.42, 0.92, 0.95, 0.7, 0.06, 0x5a5e66, 0, 1),
  (12, -0.3, 0.04, 1.55, 0.12, 0.05, 1.2, 0x8a8e96, 0, 1),
  (13, 0.3, 0.04, 1.55, 0.12, 0.05, 1.2, 0x8a8e96, 0, 1),
  (14, -0.62, 0.25, 0.45, 0.2, 0.5, 0.5, 0x181818, 0, 0),
  (15, 0.62, 0.25, 0.45, 0.2, 0.5, 0.5, 0x181818, 0, 0),
  (16, -0.62, 0.2, -0.85, 0.2, 0.4, 0.4, 0x181818, 0, 0),
  (17, 0.62, 0.2, -0.85, 0.2, 0.4, 0.4, 0x181818, 0, 0);;

INSERT INTO entities (entity_id, kind, x, y, z, yaw) VALUES
  (1, 'player', 32, 0, 18, -1.5707963267948966),
  (2, 'bot', 3, 0, 3.6, 1.5707963267948966),
  (3, 'bot', 31.5, 0, 22.8, -1.5707963267948966),
  (4, 'bot', 3, 0, 32.4, 1.5707963267948966);;

INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color, lift)
SELECT e.entity_id, m.ox, m.oy, m.oz, m.sx, m.sy, m.sz, 0,
       -- (a CASE that returns 0x.. returns its bytes, as in MariaDB: + 0 makes each a number)
       CASE WHEN m.painted = 1 THEN (CASE e.entity_id WHEN 1 THEN 0xf2c21b + 0 WHEN 2 THEN 0xe8752a + 0 WHEN 3 THEN 0xd8dde4 + 0 ELSE 0x4aa3df + 0 END)
            ELSE m.color END,
       m.lift
FROM entities e CROSS JOIN forklift_model m WHERE e.kind IN ('player', 'bot');;

-- A pallet's two boxes: the wooden base and the load, painted the SKU's colour.
DROP FUNCTION IF EXISTS pr_add_pallet_parts;;
CREATE FUNCTION pr_add_pallet_parts(a_entity INT) RETURNS INT
BEGIN
  INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color)
  VALUES (a_entity, 0, 0.07, 0, 1.1, 0.14, 1.1, 0.8, 0x9a7444);
  INSERT INTO parts (entity_id, ox, oy, oz, sx, sy, sz, rad, color)
  SELECT a_entity, 0, 0.6, 0, 1.0, 0.92, 1.0, 0.85, k.color
  FROM entities e JOIN skus k ON k.sku = e.sku WHERE e.entity_id = a_entity;
  RETURN 2;
END;;

SELECT SUM(pr_add_pallet_parts(entity_id)) FROM entities WHERE kind = 'pallet';;

UPDATE parts SET rad = 0.5 * SQRT(sx * sx + sy * sy + sz * sz);;

INSERT INTO solids (part_id, minx, maxx, minz, maxz, miny, maxy)
SELECT p.part_id, p.ox - p.sx / 2, p.ox + p.sx / 2, p.oz - p.sz / 2, p.oz + p.sz / 2, p.oy - p.sy / 2, p.oy + p.sy / 2
FROM parts p JOIN entities e ON e.entity_id = p.entity_id
WHERE e.kind = 'static' AND p.solid = 1;;

-- The aisle network: each aisle's two ends (west at x 3, east at x 31.5), and one more lane behind row F.
INSERT INTO waypoints (wp_id, x, z)
WITH RECURSIVE r (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 6),
     side (s) AS (SELECT 0 UNION ALL SELECT 1)
SELECT r.n * 2 + side.s, CASE side.s WHEN 0 THEN 3 ELSE 31.5 END, 3.6 + 4.8 * r.n FROM r CROSS JOIN side;;

INSERT INTO wp_edges (a, b)
SELECT w1.wp_id, w2.wp_id FROM waypoints w1 JOIN waypoints w2 ON w1.wp_id <> w2.wp_id
WHERE (w1.z = w2.z) OR (w1.x = w2.x AND ABS(w1.z - w2.z) < 5);;

INSERT INTO bots (entity_id, prev_wp, target_wp) VALUES (2, 0, 1), (3, 9, 8), (4, 12, 13);;

INSERT INTO game (id, tic, score, next_entity) VALUES (1, 0, 0, 2000);;

INSERT INTO events (tic, msg) VALUES (0, 'Shift started. Ship the orders, put away what comes in.');;
