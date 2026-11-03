-- Pick Rush: the whole game's state. A statement ends at `;;` in this file.
--
-- Everything in the world is a box: a part is one box placed on an entity (a rack row, a pallet, a forklift), at an
-- offset in the entity's own frame (x right, y up, z forward) and turned with it. A part with lift = 1 rides the
-- forklift's forks (its height is offset + the entity's fork_h). Units are metres, angles radians; yaw 0 faces +z.

DROP TABLE IF EXISTS box_verts;;
CREATE TABLE box_verts (
  v INT PRIMARY KEY,
  x DOUBLE NOT NULL, y DOUBLE NOT NULL, z DOUBLE NOT NULL
);;

-- The six faces of the unit box, with their outward normals.
DROP TABLE IF EXISTS box_faces;;
CREATE TABLE box_faces (
  f INT PRIMARY KEY,
  nx DOUBLE NOT NULL, ny DOUBLE NOT NULL, nz DOUBLE NOT NULL
);;

-- Two triangles a face. em marks which of a triangle's edges (bit 0: a-b, bit 1: b-c, bit 2: c-a) is a box edge,
-- so the renderer outlines the box and not the diagonal.
DROP TABLE IF EXISTS box_tris;;
CREATE TABLE box_tris (
  t INT PRIMARY KEY,
  f INT NOT NULL,
  a INT NOT NULL, b INT NOT NULL, c INT NOT NULL,
  em INT NOT NULL
);;

DROP TABLE IF EXISTS entities;;
CREATE TABLE entities (
  entity_id INT PRIMARY KEY,
  kind ENUM('static', 'player', 'bot', 'pallet') NOT NULL,
  x DOUBLE NOT NULL DEFAULT 0, y DOUBLE NOT NULL DEFAULT 0, z DOUBLE NOT NULL DEFAULT 0,
  yaw DOUBLE NOT NULL DEFAULT 0,
  speed DOUBLE NOT NULL DEFAULT 0,
  fork_h DOUBLE NOT NULL DEFAULT 0,
  sku VARCHAR(16) NULL,            -- a pallet's
  slot_id INT NULL,                -- the rack slot a pallet stands in
  carried_by INT NULL,             -- the forklift carrying a pallet
  zone VARCHAR(16) NULL,           -- 'inbound' for a pallet waiting at receiving
  KEY ent_kind (kind),
  KEY ent_carried (carried_by),
  KEY ent_slot (slot_id)
);;

DROP TABLE IF EXISTS parts;;
CREATE TABLE parts (
  part_id INT PRIMARY KEY AUTO_INCREMENT,
  entity_id INT NOT NULL,
  ox DOUBLE NOT NULL, oy DOUBLE NOT NULL, oz DOUBLE NOT NULL,
  sx DOUBLE NOT NULL, sy DOUBLE NOT NULL, sz DOUBLE NOT NULL,
  rad DOUBLE NOT NULL,             -- half the box's diagonal: the radius the culling tests
  color INT NOT NULL,              -- 0xRRGGBB
  lift INT NOT NULL DEFAULT 0,
  solid INT NOT NULL DEFAULT 0,    -- a forklift cannot drive through it (static entities only)
  KEY part_entity (entity_id)
);;

-- The solid static boxes as world-space footprints, for collision: rebuilt from parts by build_world.sql.
DROP TABLE IF EXISTS solids;;
CREATE TABLE solids (
  part_id INT PRIMARY KEY,
  minx DOUBLE NOT NULL, maxx DOUBLE NOT NULL,
  minz DOUBLE NOT NULL, maxz DOUBLE NOT NULL,
  miny DOUBLE NOT NULL, maxy DOUBLE NOT NULL,
  KEY solid_x (minx)
);;

-- Rack locations, the way the ERP names bins: row letter, bay, level (A-03-2).
DROP TABLE IF EXISTS slots;;
CREATE TABLE slots (
  slot_id INT PRIMARY KEY,
  label VARCHAR(16) NOT NULL,
  rack_row INT NOT NULL, bay INT NOT NULL, level INT NOT NULL,
  x DOUBLE NOT NULL, y DOUBLE NOT NULL, z DOUBLE NOT NULL,
  pallet_id INT NULL,
  KEY slot_pallet (pallet_id)
);;

DROP TABLE IF EXISTS skus;;
CREATE TABLE skus (
  sku VARCHAR(16) PRIMARY KEY,
  name VARCHAR(40) NOT NULL,
  color INT NOT NULL
);;

DROP TABLE IF EXISTS orders;;
CREATE TABLE orders (
  order_id INT PRIMARY KEY AUTO_INCREMENT,
  created_tic INT NOT NULL,
  due_tic INT NOT NULL,
  status ENUM('open', 'shipped', 'late') NOT NULL DEFAULT 'open',
  closed_tic INT NULL,
  KEY order_status (status)
);;

DROP TABLE IF EXISTS order_lines;;
CREATE TABLE order_lines (
  order_id INT NOT NULL,
  line_no INT NOT NULL,
  sku VARCHAR(16) NOT NULL,
  need INT NOT NULL,
  picked INT NOT NULL DEFAULT 0,
  PRIMARY KEY (order_id, line_no)
);;

-- The aisle network the other forklifts drive: waypoints and the aisles between them.
DROP TABLE IF EXISTS waypoints;;
CREATE TABLE waypoints (
  wp_id INT PRIMARY KEY,
  x DOUBLE NOT NULL, z DOUBLE NOT NULL
);;

DROP TABLE IF EXISTS wp_edges;;
CREATE TABLE wp_edges (
  a INT NOT NULL, b INT NOT NULL,
  PRIMARY KEY (a, b)
);;

DROP TABLE IF EXISTS bots;;
CREATE TABLE bots (
  entity_id INT PRIMARY KEY,
  prev_wp INT NOT NULL,
  target_wp INT NOT NULL,
  wait_tics INT NOT NULL DEFAULT 0,
  held_tics INT NOT NULL DEFAULT 0   -- how long someone in front has held it up
);;

-- One row: the clock, the score and what the player asked for this tic.
DROP TABLE IF EXISTS game;;
CREATE TABLE game (
  id INT PRIMARY KEY,
  tic INT NOT NULL DEFAULT 0,
  score INT NOT NULL DEFAULT 0,
  shipped INT NOT NULL DEFAULT 0,
  late INT NOT NULL DEFAULT 0,
  putaways INT NOT NULL DEFAULT 0,
  next_entity INT NOT NULL DEFAULT 1000
);;

-- What happened, for the HUD: the newest few are shown.
DROP TABLE IF EXISTS events;;
CREATE TABLE events (
  event_id INT PRIMARY KEY AUTO_INCREMENT,
  tic INT NOT NULL,
  msg VARCHAR(120) NOT NULL
);;
