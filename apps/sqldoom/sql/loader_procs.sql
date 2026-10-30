-- SQLDoom's import-time materializations (sql/loader/functions), as ASQL stored procedures. A statement ends at `;;`
-- in this file (the loader splits there), as each procedure body holds `;`.

DROP PROCEDURE IF EXISTS doom_materialize_render_segs;;
CREATE PROCEDURE doom_materialize_render_segs(p_map_id INT)
BEGIN
DELETE FROM render_segs WHERE map_id=p_map_id;

INSERT INTO render_segs (
  map_id,seg_id,ssector_id,direction,linedef_id,x1,y1,x2,y2,
  seg_u1,seg_u2,fsec,bsec,x_offset,y_offset,
  upper_tex,mid_tex,lower_tex,flags,
  f_floor,f_ceil,f_ceil_tex,f_light,
  b_floor,b_ceil,b_ceil_tex,b_light,light_bias
)
SELECT
  sg.map_id,sg.id,ss.id,sg.direction,sg.linedef_id,
  v1.x,v1.y,v2.x,v2.y,
  CASE WHEN sg.direction=0 THEN sg.offs
       ELSE sg.offs+SQRT(POW(v2.x-v1.x,2)+POW(v2.y-v1.y,2)) END,
  CASE WHEN sg.direction=0
       THEN sg.offs+SQRT(POW(v2.x-v1.x,2)+POW(v2.y-v1.y,2))
       ELSE sg.offs END,
  fs.id,bs.id,front.x_offset,front.y_offset,
  front.upper_tex,front.mid_tex,front.lower_tex,ld.flags,
  fs.floor_height,fs.ceil_height,fs.ceil_tex,fs.light_level,
  bs.floor_height,bs.ceil_height,bs.ceil_tex,bs.light_level,
  CASE WHEN v1.y=v2.y THEN -1 WHEN v1.x=v2.x THEN 1 ELSE 0 END
FROM segs sg
JOIN ssectors ss ON ss.map_id=sg.map_id
  AND sg.id>=ss.first_seg_id
  AND sg.id<ss.first_seg_id+ss.seg_count
JOIN vertexes v1 ON v1.map_id=sg.map_id AND v1.id=sg.v1_id
JOIN vertexes v2 ON v2.map_id=sg.map_id AND v2.id=sg.v2_id
JOIN linedefs ld ON ld.map_id=sg.map_id AND ld.id=sg.linedef_id
JOIN sidedefs front ON front.map_id=sg.map_id
  AND front.id=CASE WHEN sg.direction=0 THEN ld.right_sd_id ELSE ld.left_sd_id END
LEFT JOIN sidedefs back ON back.map_id=sg.map_id
  AND back.id=CASE WHEN sg.direction=0 THEN ld.left_sd_id ELSE ld.right_sd_id END
JOIN sectors fs ON fs.map_id=sg.map_id AND fs.id=front.sector_id
LEFT JOIN sectors bs ON bs.map_id=sg.map_id AND bs.id=back.sector_id
WHERE sg.map_id=p_map_id;

DELETE FROM sector_sound_origins WHERE map_id=p_map_id;

INSERT INTO sector_sound_origins(map_id,sector_id,x,y)
SELECT map_id,sector_id,AVG(x),AVG(y)
FROM (
  SELECT map_id,fsec AS sector_id,x1 AS x,y1 AS y
  FROM render_segs WHERE map_id=p_map_id
  UNION ALL
  SELECT map_id,bsec,x1,y1
  FROM render_segs WHERE map_id=p_map_id AND bsec IS NOT NULL
) boundary
GROUP BY map_id,sector_id;
END;;

DROP PROCEDURE IF EXISTS doom_materialize_render_things;;
-- Each Thing's subsector, found by walking the BSP from the root on the side of each partition line it is on.
CREATE PROCEDURE doom_materialize_render_things(p_map_id INT)
BEGIN
DELETE FROM render_things WHERE map_id=p_map_id;

INSERT INTO render_things (
  map_id,thing_id,sector_id,spawn_sector_id,sprite,frame,fullbright,
  spawn_ceiling,thing_height
)
WITH RECURSIVE
roots AS (
  SELECT map_id,MAX(id) AS node_id
  FROM nodes
  WHERE map_id=p_map_id
  GROUP BY map_id
),
thing_bsp AS (
  SELECT t.map_id,t.id AS thing_id,t.x AS tx,t.y AS ty,
         r.node_id,CAST(NULL AS SIGNED) AS ssector_id
  FROM things t
  JOIN thing_sprite_defs d ON d.thing_type=t.type
  JOIN roots r ON r.map_id=t.map_id
  WHERE t.map_id=p_map_id

  UNION ALL

  SELECT b.map_id,b.thing_id,b.tx,b.ty,
         CASE WHEN nc.child_kind='NODE' THEN nc.child_node_id END,
         CASE WHEN nc.child_kind='SSECTOR' THEN nc.child_ssector_id END
  FROM thing_bsp b
  JOIN nodes n ON n.map_id=b.map_id AND n.id=b.node_id
  JOIN node_children nc
    ON nc.map_id=b.map_id AND nc.node_id=n.id
   AND nc.side=CASE
     WHEN (b.tx-n.x)*n.dy-(b.ty-n.y)*n.dx>0
     THEN 'R' ELSE 'L' END
  WHERE b.node_id IS NOT NULL
),
located AS (
  SELECT b.map_id,b.thing_id,rs.fsec AS sector_id,
         ROW_NUMBER() OVER (
           PARTITION BY b.map_id,b.thing_id ORDER BY rs.seg_id
         ) AS rn
  FROM thing_bsp b
  JOIN render_segs rs
    ON rs.map_id=b.map_id AND rs.ssector_id=b.ssector_id
  WHERE b.ssector_id IS NOT NULL
)
SELECT l.map_id,l.thing_id,l.sector_id,l.sector_id,
       d.sprite,d.frame,d.fullbright,d.spawn_ceiling,d.thing_height
FROM located l
JOIN things t ON t.map_id=l.map_id AND t.id=l.thing_id
JOIN thing_sprite_defs d ON d.thing_type=t.type
WHERE l.rn=1;
END;;
