-- Tables derived from the loaded WAD. SQLDoom keeps these as PostgreSQL materialized views refreshed after an import;
-- ASQL has no materialized views, so the loader rebuilds each one as a table once the maps are in.

DROP TABLE IF EXISTS sector_adjacency;
-- Which sectors touch which, as an edge list, both ways round.
CREATE TABLE sector_adjacency AS
SELECT ld.map_id, ld.id AS linedef_id, r.sector_id, l.sector_id AS other_id
FROM linedefs ld
JOIN sidedefs r ON r.map_id = ld.map_id AND r.id = ld.right_sd_id
JOIN sidedefs l ON l.map_id = ld.map_id AND l.id = ld.left_sd_id
UNION ALL
SELECT ld.map_id, ld.id, l.sector_id, r.sector_id
FROM linedefs ld
JOIN sidedefs r ON r.map_id = ld.map_id AND r.id = ld.right_sd_id
JOIN sidedefs l ON l.map_id = ld.map_id AND l.id = ld.left_sd_id;
CREATE INDEX idx_sector_adjacency ON sector_adjacency (map_id, sector_id);

DROP TABLE IF EXISTS linedef_geom;
-- Linedef -> vertexes -> sidedefs -> sector ids, flattened once.
CREATE TABLE linedef_geom AS
SELECT ld.map_id, ld.id AS linedef_id, v1.x AS x1, v1.y AS y1,
       v2.x AS x2, v2.y AS y2, ld.flags, ld.special,
       ld.right_sd_id, ld.left_sd_id,
       -- fsec/bsec are LEFT JOIN results: NULL when the line is one-sided.
       rs.sector_id AS fsec, ls.sector_id AS bsec
FROM linedefs ld
JOIN vertexes v1 ON v1.map_id=ld.map_id AND v1.id=ld.v1_id
JOIN vertexes v2 ON v2.map_id=ld.map_id AND v2.id=ld.v2_id
LEFT JOIN sidedefs rs ON rs.map_id=ld.map_id AND rs.id=ld.right_sd_id
LEFT JOIN sidedefs ls ON ls.map_id=ld.map_id AND ls.id=ld.left_sd_id;

DROP TABLE IF EXISTS node_path_steps;
-- Every subsector's path from the BSP root: one row per ancestor node, with the side taken to reach the subsector,
-- depth 1 at the root. SQLDoom walks down from the root collecting arrays; here the walk goes up from each
-- subsector (a node has one parent), and the depth from the root is the path's length less the steps taken up.
CREATE TABLE node_path_steps AS
WITH RECURSIVE up_walk AS (
  SELECT nc.map_id, nc.child_ssector_id AS ssector_id, nc.node_id, nc.side, 1 AS up
  FROM node_children nc
  WHERE nc.child_kind = 'SSECTOR'
  UNION ALL
  SELECT u.map_id, u.ssector_id, p.node_id, p.side, u.up + 1
  FROM up_walk u
  JOIN node_children p ON p.map_id = u.map_id AND p.child_kind = 'NODE' AND p.child_node_id = u.node_id
)
SELECT map_id, ssector_id,
       MAX(up) OVER (PARTITION BY map_id, ssector_id) - up + 1 AS depth,
       node_id, side
FROM up_walk;
CREATE INDEX idx_node_path_steps ON node_path_steps (map_id, ssector_id);

DROP TABLE IF EXISTS ui_hud_pixels;
-- The status bar's own patches, split out of ui_patch_pixels so the renderer does not scan the menu art.
CREATE TABLE ui_hud_pixels AS
SELECT name, dx, dy, palette_index
FROM ui_patch_pixels WHERE name LIKE 'ST%';
CREATE INDEX idx_ui_hud_pixels ON ui_hud_pixels (name);

DROP TABLE IF EXISTS ui_static_pixels;
CREATE TABLE ui_static_pixels AS
WITH static_layers AS (
  SELECT 'STBAR' AS name, 0 AS layer, 0 AS x, 168 AS y
  UNION ALL SELECT 'STTPRCNT', 1, 90, 172
  UNION ALL SELECT 'STTPRCNT', 1, 221, 172
  UNION ALL SELECT 'STARMS', 1, 104, 168
), candidates AS (
  SELECT l.layer,
         l.x-p.left_offset+px.dx AS x,
         l.y-p.top_offset+px.dy AS y,
         px.palette_index,
         ROW_NUMBER() OVER (
           PARTITION BY l.x-p.left_offset+px.dx,
                        l.y-p.top_offset+px.dy
           ORDER BY l.layer DESC,l.name
         ) AS rn
  FROM static_layers l
  JOIN ui_patches p ON p.name=l.name
  JOIN ui_patch_pixels px ON px.name=l.name
)
SELECT c.x, c.y, cm.rgb, c.palette_index
FROM candidates c
JOIN colormap_rgb cm
  ON cm.pal=0 AND cm.level=0 AND cm.palette_index=c.palette_index
WHERE c.rn=1;
