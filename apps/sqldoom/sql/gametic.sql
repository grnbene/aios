-- SQLDoom's game tic (github.com/cedardb/sqldoom, sql/runtime/functions, GPL-2.0) as ASQL stored functions.
-- Converted from CedarScript by a script and then edited by hand where it said to; see README.md. Run under
-- sql_mode 'PIPES_AS_CONCAT,PG_DIVISION' (each function keeps it), as the SQL inside is PostgreSQL's.
-- A statement ends at `;;` in this file.

-- ==== 00_prandom.sql ====
-- Doom's P_Random, as far as a set-based engine can have one.
--
-- m_random.c draws rndtable[++prndindex & 0xff]: one global table, walked in
-- whatever order the thinker list runs. There is no such order here -- every
-- monster on the map is one row of one statement, evaluated as a set -- so a
-- walking index cannot be reproduced, and anything recorded against it could
-- never be replayed. The table below is Doom's; the index into it is derived
-- instead, from (actor, tic, call site). That is reproducible by
-- construction: the same world, ticked again on the same input, draws the
-- same numbers however the rows happen to be evaluated.
--
-- p_use separates call sites, so two draws for one actor in one tic -- a
-- shot's spread and its damage roll, say -- are independent rather than equal.
--
-- The three multipliers are large odd 32-bit constants and the sum goes
-- through an xor-shift-multiply before a byte is taken out of the middle.
-- Small multipliers are not enough on their own: a tic counter times 40503
-- leaves the first tics below the bits being extracted, and tic 0 and tic 1
-- drew the same number. The final multiplier is deliberately 16-bit so the
-- product stays inside a signed 64-bit integer.
-- Ported by hand: PostgreSQL's # (xor) is MySQL's ^, which binds tighter, so the operands are bracketed; the integer
-- divisions are DIV.
DROP FUNCTION IF EXISTS doom_prandom;;
CREATE FUNCTION doom_prandom(a_actor BIGINT, a_tic BIGINT, a_use BIGINT) RETURNS INT DETERMINISTIC
RETURN GET_BYTE(
    X'00086ddcdef1956b4bf8fe8c10424a15d32f50f29a1bcd80a1594d245f6e5530d48cd3f9164fc8321cbc348cca7844913e46b8be5bc598e0956819b2fcb6cab68dc50451b5f2912a27e39cc6e1c1db5d7aaff900af8f46ef2ef6a335a36da88702eb195c14918a4d45a64eb0add4a6715ea12932ef316fa4463c0225ab4b889c0b382a928ae549924d3d62c4876a3fc5c35660cb7165aaf7b57150fa6c07ffed81e24f6b70a667f118dfef78c63a3c528003b8428fe091e051cea32d3f5aa8723b219f5f1c8b7b627dc40f46c2fd360e6de24711a15dba57f48a14347bfb1a24112e34e7e84c1fdd5425d8a5d46ac5f2622b27affe91be5476debb8878a3ecf9',
    (((ABS(a_actor) * 2654435761 + ABS(a_tic) * 2246822519 + ABS(a_use) * 3266489917) % 4294967296)
     ^ (((ABS(a_actor) * 2654435761 + ABS(a_tic) * 2246822519 + ABS(a_use) * 3266489917) % 4294967296) DIV 32768)
    ) * 40503 % 4294967296 DIV 256 % 256);;

-- ==== 01_constants.sql ====
-- A named constant from doom_constants, for CedarScript to read into a
-- variable at function entry: let friction = doom_const('FRICTION');
DROP FUNCTION IF EXISTS doom_const;;
CREATE FUNCTION doom_const(a_name TEXT) RETURNS DOUBLE
BEGIN
  DECLARE v_v DOUBLE;
  DECLARE v_x DOUBLE;
  SET v_v = 0::double precision;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT value AS x FROM doom_constants WHERE name = a_name;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_v = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_v;
END;;

-- ==== 02_geometry.sql ====
-- Recursive bsp tree descend to find the sector
-- on the given map where the givoin point is in
DROP FUNCTION IF EXISTS doom_sector_at;;
CREATE FUNCTION doom_sector_at(a_map_id INT, a_x DOUBLE, a_y DOUBLE) RETURNS INT
BEGIN
  DECLARE v_sector INT;
  DECLARE v_f BIGINT;
  SET v_sector = NULL::int;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR WITH RECURSIVE bsp AS (
        -- The root is the highest-numbered node
        (SELECT 1 AS depth, n.id AS node_id, NULL::int AS ssector_id
         FROM nodes n WHERE n.map_id = a_map_id
         ORDER BY n.id DESC LIMIT 1)
        UNION ALL
        SELECT w.depth + 1,
               CASE WHEN nc.child_kind = 'NODE' THEN nc.child_node_id END,
               CASE WHEN nc.child_kind = 'SSECTOR' THEN nc.child_ssector_id END
        FROM bsp w
        JOIN nodes n ON n.map_id = a_map_id AND n.id = w.node_id
        JOIN node_children nc ON nc.map_id = a_map_id AND nc.node_id = n.id
          AND nc.side = CASE
            WHEN (a_x - n.x) * n.dy::float8 - (a_y - n.y) * n.dx::float8 > 0
            THEN 'R' ELSE 'L' END
        WHERE w.node_id IS NOT NULL
      )
      SELECT rs.fsec AS f
      FROM bsp b
      JOIN render_segs rs ON rs.map_id = a_map_id AND rs.ssector_id = b.ssector_id
      WHERE b.ssector_id IS NOT NULL
      ORDER BY rs.seg_id
      LIMIT 1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_f;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_sector = v_f;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_sector;
END;;

-- ==== 03_weapon_decision.sql ====

CREATE OR REPLACE VIEW doom_weapon_decision AS
-- Advance the player's weapon psprite state machine one 35 Hz tic: up/down
-- (switching), ready (idle bob), fire (attack sequence with vanilla-style
-- refire), and an independent muzzle-flash overlay sequence.
-- Params: map_id, player_thing_id.
WITH
input AS (
  SELECT g.map_id,g.player_thing_id,g.attack_held,
         COALESCE(g.weapon_switch_to,pg.weapon_id) AS switch_to,
         ps.bob_strength AS bob,ps.level_tics::int AS level_tic
  FROM game_tic_commands g
  JOIN player_state ps ON ps.map_id=g.map_id
    AND ps.player_thing_id=g.player_thing_id
  LEFT JOIN (
    SELECT map_id,player_thing_id,MIN(weapon_id) AS weapon_id
    FROM pickup_grants GROUP BY map_id,player_thing_id
  ) pg ON pg.map_id=g.map_id AND pg.player_thing_id=g.player_thing_id
),
current AS (
  SELECT pw.*, wd.ammo_type, wd.ammo_per_shot, i.attack_held, i.switch_to,
         i.bob, i.level_tic
  FROM input i
  JOIN player_weapons pw
    ON pw.map_id = i.map_id AND pw.player_thing_id = i.player_thing_id
  JOIN weapon_defs wd ON wd.weapon_id = pw.current_weapon
),
ammo AS (
  SELECT c.*,
    (CASE c.ammo_type
       WHEN 'bullets' THEN ps.ammo_bullets
       WHEN 'shells'  THEN ps.ammo_shells
       WHEN 'rockets' THEN ps.ammo_rockets
       WHEN 'cells'   THEN ps.ammo_cells
       ELSE 999999
     END >= c.ammo_per_shot) AS has_ammo
  FROM current c
  JOIN player_state ps ON ps.map_id = c.map_id AND ps.player_thing_id = c.player_thing_id
),
switch AS (
  SELECT a.*,
    (a.switch_to IS NOT NULL AND a.switch_to <> a.current_weapon
     AND own.weapon_id IS NOT NULL) AS switch_requested
  FROM ammo a
  LEFT JOIN player_weapon_owned own
    ON own.map_id = a.map_id AND own.player_thing_id = a.player_thing_id
   AND own.weapon_id = a.switch_to
),
frames AS (
  SELECT s.*,
         ready0.tics AS ready_tics,
         fire0.tics AS fire0_tics, fire0.is_attack_frame AS fire0_attack,
         nextf.seq_index AS next_seq, nextf.tics AS next_tics,
         nextf.is_attack_frame AS next_attack, nextf.refire_check AS next_refire,
         flash0.tics AS flash0_tics,
         flashnext.seq_index AS flashnext_seq, flashnext.tics AS flashnext_tics
  FROM switch s
  LEFT JOIN weapon_frames ready0
    ON ready0.weapon_id = s.current_weapon AND ready0.state = 'ready' AND ready0.seq_index = 0
  LEFT JOIN weapon_frames fire0
    ON fire0.weapon_id = s.current_weapon AND fire0.state = 'fire' AND fire0.seq_index = 0
  LEFT JOIN weapon_frames nextf
    ON nextf.weapon_id = s.current_weapon AND nextf.state = 'fire' AND nextf.seq_index = s.seq_index + 1
  LEFT JOIN weapon_frames flash0
    ON flash0.weapon_id = s.current_weapon AND flash0.state = 'flash' AND flash0.seq_index = 0
  LEFT JOIN weapon_frames flashnext
    ON flashnext.weapon_id = s.current_weapon AND flashnext.state = 'flash'
   AND flashnext.seq_index = s.flash_seq_index + 1
),
flags AS (
  SELECT f.*,
    (f.state = 'fire' AND f.tics <= 1) AS advancing,
    (f.state = 'fire' AND f.tics <= 1 AND f.next_seq IS NULL) AS falls_off,
    (f.state = 'fire' AND f.tics <= 1 AND f.next_seq IS NOT NULL
       AND f.next_refire AND f.attack_held AND f.has_ammo) AS refires,
    (f.state = 'ready' AND f.attack_held AND f.has_ammo
       AND NOT f.switch_requested) AS starts_fire,
    (f.state = 'down' AND f.sy + 6.0 >= 128.0) AS down_done,
    (f.state = 'up' AND f.sy - 6.0 <= 32.0) AS up_done,
    (f.flash_seq_index IS NOT NULL AND f.flash_tics <= 1) AS flash_advancing
  FROM frames f
),
decision AS (
  SELECT fl.*,
    CASE
      WHEN fl.switch_requested THEN 'down'::weapon_state
      WHEN fl.state = 'down' THEN CASE WHEN fl.down_done THEN 'up'::weapon_state ELSE 'down'::weapon_state END
      WHEN fl.state = 'up' THEN CASE WHEN fl.up_done THEN 'ready'::weapon_state ELSE 'up'::weapon_state END
      WHEN fl.state = 'ready' THEN CASE WHEN fl.starts_fire THEN 'fire'::weapon_state ELSE 'ready'::weapon_state END
      WHEN fl.state = 'fire' AND fl.advancing AND fl.falls_off THEN 'ready'::weapon_state
      ELSE fl.state
    END AS n_state,
    CASE
      WHEN fl.switch_requested THEN fl.seq_index
      WHEN fl.state = 'up' AND fl.up_done THEN 0
      WHEN fl.state = 'ready' THEN 0
      WHEN fl.state = 'fire' AND fl.advancing THEN
        CASE WHEN fl.falls_off OR fl.refires THEN 0 ELSE fl.next_seq END
      ELSE fl.seq_index
    END AS n_seq,
    CASE
      WHEN fl.switch_requested THEN fl.tics
      WHEN fl.state = 'up' THEN CASE WHEN fl.up_done THEN fl.ready_tics ELSE fl.tics END
      WHEN fl.state = 'ready' THEN
        CASE WHEN fl.starts_fire THEN fl.fire0_tics ELSE fl.tics END
      WHEN fl.state = 'fire' AND fl.advancing THEN
        CASE
          WHEN fl.falls_off THEN fl.ready_tics
          WHEN fl.refires THEN fl.fire0_tics
          ELSE fl.next_tics
        END
      WHEN fl.state = 'fire' THEN fl.tics - 1
      ELSE fl.tics
    END AS n_tics,
    -- fires_now: true only the single tic an is_attack_frame row is entered.
    (
      (fl.state = 'ready' AND fl.starts_fire AND fl.fire0_attack IS TRUE)
      OR (fl.state = 'fire' AND fl.advancing AND NOT fl.falls_off
          AND NOT fl.refires AND fl.next_attack IS TRUE)
      -- A_ReFire jumps straight back to fire/0. If that first state owns the
      -- action pointer (chaingun/plasma/chainsaw), entering it fires now too.
      OR (fl.state = 'fire' AND fl.advancing AND fl.refires
          AND fl.fire0_attack IS TRUE)
    ) AS fires_now,
    CASE WHEN fl.state = 'down' AND fl.down_done AND NOT fl.switch_requested
         THEN fl.pending_weapon ELSE fl.current_weapon END AS n_current_weapon,
    CASE
      WHEN fl.switch_requested THEN fl.switch_to
      WHEN fl.state = 'down' AND fl.down_done THEN NULL
      ELSE fl.pending_weapon
    END AS n_pending_weapon
  FROM flags fl
),
decision2 AS (
  SELECT d.*,
    CASE WHEN d.n_state = 'ready' THEN 1.0 + d.bob * COS(2.0*PI()*(d.level_tic % 64)/64.0)
         ELSE d.sx END AS n_sx,
    CASE
      WHEN d.n_state = 'ready' THEN 32.0 + d.bob * SIN(2.0*PI()*(d.level_tic % 32)/64.0)
      WHEN d.n_state = 'down' THEN LEAST(128.0, d.sy + 6.0)
      WHEN d.n_state = 'up' THEN GREATEST(32.0, d.sy - 6.0)
      ELSE d.sy
    END AS n_sy,
    CASE
      WHEN d.fires_now THEN CASE WHEN d.flash0_tics IS NOT NULL THEN 0 END
      WHEN d.flash_seq_index IS NULL THEN NULL
      WHEN NOT d.flash_advancing THEN d.flash_seq_index
      ELSE d.flashnext_seq
    END AS n_flash_seq,
    CASE
      WHEN d.fires_now THEN COALESCE(d.flash0_tics, 0)
      WHEN d.flash_seq_index IS NULL THEN 0
      WHEN NOT d.flash_advancing THEN d.flash_tics - 1
      ELSE COALESCE(d.flashnext_tics, 0)
    END AS n_flash_tics
  FROM decision d
)
SELECT map_id,player_thing_id,n_state,n_seq,n_tics,n_current_weapon,
       n_pending_weapon,n_flash_seq,n_flash_tics,n_sx,n_sy,fires_now
FROM decision2;;

-- ==== 04_reset_stage.sql ====
DROP FUNCTION IF EXISTS doom_reset_stage;;
CREATE FUNCTION doom_reset_stage(a_map_id INT, a_skill INT) RETURNS INT
BEGIN
  -- Restore a stage's mutable sector geometry and clear its thinkers.
  UPDATE sectors
  SET floor_height = spawn_floor_height,
      ceil_height = spawn_ceil_height,
      floor_tex = spawn_floor_tex,
      light_level = COALESCE(spawn_light_level, light_level)
  WHERE map_id = a_map_id;
  DELETE FROM sector_light_fx WHERE map_id = a_map_id;
  INSERT INTO sector_light_fx (map_id, sector_id, special, base_light, dark_light)
  SELECT s.map_id, s.id, s.special::smallint,
         COALESCE(s.spawn_light_level, s.light_level)::smallint,
         LEAST(COALESCE(s.spawn_light_level, s.light_level),
               COALESCE(MIN(other.light_level), 0::smallint))::smallint
  FROM sectors s
  LEFT JOIN linedefs edge ON edge.map_id = s.map_id
  LEFT JOIN sidedefs r ON r.map_id = edge.map_id AND r.id = edge.right_sd_id
  LEFT JOIN sidedefs l ON l.map_id = edge.map_id AND l.id = edge.left_sd_id
  LEFT JOIN sectors other ON other.map_id = s.map_id
    AND other.id = CASE WHEN r.sector_id = s.id THEN l.sector_id
                        WHEN l.sector_id = s.id THEN r.sector_id END
  WHERE s.map_id = a_map_id AND s.special IN (1,2,3,8,12,13,17)
  GROUP BY s.map_id, s.id, s.special, s.spawn_light_level, s.light_level;
  UPDATE render_segs rs
  SET f_floor = s.floor_height, f_ceil = s.ceil_height,
      f_ceil_tex = s.ceil_tex, f_light = s.light_level
  FROM sectors s
  WHERE rs.map_id = a_map_id AND s.map_id = rs.map_id
    AND rs.fsec = s.id
    AND (rs.f_floor <> s.floor_height OR rs.f_ceil <> s.ceil_height
      OR rs.f_ceil_tex IS DISTINCT FROM s.ceil_tex OR rs.f_light <> s.light_level);
  UPDATE render_segs rs
  SET b_floor = s.floor_height, b_ceil = s.ceil_height,
      b_ceil_tex = s.ceil_tex, b_light = s.light_level
  FROM sectors s
  WHERE rs.map_id = a_map_id AND s.map_id = rs.map_id
    AND rs.bsec = s.id
    AND (rs.b_floor <> s.floor_height OR rs.b_ceil <> s.ceil_height
      OR rs.b_ceil_tex IS DISTINCT FROM s.ceil_tex OR rs.b_light <> s.light_level);
  DELETE FROM sector_movers WHERE map_id = a_map_id;
  -- Restore any switch/button textures before clearing their runtime rows.
  WITH original AS (
    SELECT DISTINCT map_id,sidedef_id FROM line_buttons WHERE map_id=a_map_id
  )
  UPDATE sidedefs sd
  SET upper_tex=COALESCE(u.original_tex,sd.upper_tex),
      mid_tex=COALESCE(m.original_tex,sd.mid_tex),
      lower_tex=COALESCE(l.original_tex,sd.lower_tex)
  FROM original o
  LEFT JOIN line_buttons u ON u.map_id=o.map_id AND u.sidedef_id=o.sidedef_id
    AND u.texture_part='upper'
  LEFT JOIN line_buttons m ON m.map_id=o.map_id AND m.sidedef_id=o.sidedef_id
    AND m.texture_part='middle'
  LEFT JOIN line_buttons l ON l.map_id=o.map_id AND l.sidedef_id=o.sidedef_id
    AND l.texture_part='lower'
  WHERE sd.map_id=o.map_id AND sd.id=o.sidedef_id;
  UPDATE render_segs rs
  SET upper_tex=sd.upper_tex, mid_tex=sd.mid_tex, lower_tex=sd.lower_tex,
      x_offset=sd.x_offset,y_offset=sd.y_offset
  FROM linedefs ld,sidedefs sd
  WHERE rs.map_id=a_map_id AND ld.map_id=rs.map_id AND ld.id=rs.linedef_id
    AND sd.map_id=ld.map_id
    AND sd.id=CASE WHEN rs.direction=0 THEN ld.right_sd_id ELSE ld.left_sd_id END;
  DELETE FROM line_buttons WHERE map_id = a_map_id;
  DELETE FROM line_activations WHERE map_id = a_map_id;
  DELETE FROM line_special_events WHERE map_id = a_map_id;
  DELETE FROM line_use_results WHERE map_id = a_map_id;
  DELETE FROM pickup_grants WHERE map_id = a_map_id;
  DELETE FROM sound_events WHERE map_id = a_map_id;
  DELETE FROM mapped_lines WHERE map_id = a_map_id;
  -- Dropped items from monster deaths are transient
  -- they only ever existed this playthrough, so they don't get a spawn_x/y
  -- to restore like real WAD Things, so just remove them outright.
  -- They are fortunately easy to recognize (source_thing_id + 100000)
  DELETE FROM render_things WHERE map_id = a_map_id AND thing_id >= 100000;
  DELETE FROM things WHERE map_id = a_map_id AND id >= 100000;
  -- Move monsters back to their spawn
  UPDATE things
  SET x = spawn_x, y = spawn_y, angle = spawn_angle,
      mom_x = 0, mom_y = 0
  WHERE map_id = a_map_id;
  UPDATE render_things
  SET sector_id = spawn_sector_id
  WHERE map_id = a_map_id AND spawn_sector_id IS NOT NULL AND sector_id <> spawn_sector_id;
  -- Floaters keep their height in z
  UPDATE things t
  SET z = s.floor_height
  FROM render_things rt
  JOIN sectors s ON s.map_id = rt.map_id AND s.id = rt.sector_id
  JOIN thing_combat_defs d ON d.thing_type = t.type AND d.floats
  WHERE t.map_id = a_map_id
    AND rt.map_id = t.map_id AND rt.thing_id = t.id;
  DELETE FROM thing_health WHERE map_id = a_map_id;
  DELETE FROM monster_deaths WHERE map_id = a_map_id;
  DELETE FROM monster_steps WHERE map_id = a_map_id;
  DELETE FROM automap_view WHERE map_id = a_map_id;
  DELETE FROM monster_attack_damage WHERE map_id = a_map_id;
  DELETE FROM monster_teleports WHERE map_id = a_map_id;
  INSERT INTO thing_health (map_id, thing_id, health, max_health, alive)
  SELECT t.map_id, t.id, d.spawn_health, d.spawn_health, TRUE
  FROM things t
  JOIN thing_combat_defs d ON d.thing_type = t.type
  CROSS JOIN (SELECT CASE WHEN a_skill<=1 THEN 1
                          WHEN a_skill=2 THEN 2 ELSE 4 END AS bit) spawn
  WHERE t.map_id = a_map_id AND (t.flags & spawn.bit) <> 0
    AND (t.flags & 16) = 0;
  DELETE FROM monster_ai WHERE map_id = a_map_id;
  INSERT INTO monster_ai (map_id, thing_id, state, state_tics, seq_index,
                           sector_id, attack_cooldown, fired_this_tick)
  SELECT map_id, thing_id, 'stand', -1, 0, NULL, 0, FALSE
  FROM thing_health WHERE map_id = a_map_id;
  DELETE FROM player_weapons WHERE map_id = a_map_id;
  INSERT INTO player_weapons (map_id, player_thing_id, state, sy)
  SELECT t.map_id, t.id, 'ready', 32.0 FROM things t
  JOIN thing_role_defs r ON r.thing_type = t.type AND r.player_number = 1
  WHERE t.map_id = a_map_id;
  -- Vanilla starts every player with the fist and pistol (weapon_id 1, 2).
  DELETE FROM player_weapon_owned WHERE map_id = a_map_id;
  INSERT INTO player_weapon_owned (map_id, player_thing_id, weapon_id)
  SELECT t.map_id, t.id, w.weapon_id
  FROM things t CROSS JOIN (VALUES (1), (2)) AS w (weapon_id)
  JOIN thing_role_defs r ON r.thing_type = t.type AND r.player_number = 1
  WHERE t.map_id = a_map_id;
  DELETE FROM player_state WHERE map_id = a_map_id;
  -- 50 starting bullets
  INSERT INTO player_state (
    map_id,player_thing_id,health,alive,ammo_bullets,
    previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle
  )
  SELECT t.map_id,t.id,100,TRUE,50,t.x,t.y,t.x,t.y,t.z,t.z,t.angle
  FROM things t
  JOIN thing_role_defs r ON r.thing_type = t.type AND r.player_number = 1
  WHERE t.map_id = a_map_id;
  DELETE FROM world_effects WHERE map_id = a_map_id;
  DELETE FROM monster_projectiles WHERE map_id = a_map_id;
  DELETE FROM projectile_impacts WHERE map_id = a_map_id;
  DELETE FROM projectile_damage WHERE map_id = a_map_id;
  DELETE FROM hitscan_hits WHERE map_id = a_map_id;
  DELETE FROM picked_up_items WHERE map_id = a_map_id;
  DELETE FROM item_respawns WHERE map_id = a_map_id;
  DELETE FROM pickup_touches WHERE map_id = a_map_id;
  DELETE FROM game_tic_commands WHERE map_id = a_map_id;
  -- Start a fresh intermission record.
  DELETE FROM level_secret_discoveries WHERE map_id = a_map_id;
  DELETE FROM level_stats WHERE map_id = a_map_id;
  WITH params AS (
    SELECT a_map_id::int AS map_id,
           CASE WHEN a_skill<=1 THEN 1
                WHEN a_skill=2 THEN 2 ELSE 4 END AS skill_bit
  )
  INSERT INTO level_stats (
    map_id,player_thing_id,skill_bit,par_tics,
    total_kills,total_items,total_secrets
  )
  SELECT
    t.map_id,t.id,p.skill_bit,
    (SELECT lp.par_secs*35 FROM level_pars lp
      WHERE lp.episode = substring(m.name FROM 2 FOR 1)::int
        AND lp.level   = substring(m.name FROM 4 FOR 1)::int),
    (SELECT count(*)
     FROM things mt
     JOIN thing_combat_defs cd ON cd.thing_type=mt.type AND cd.counts_kill
     WHERE mt.map_id=p.map_id
       AND (mt.flags & p.skill_bit)<>0 AND (mt.flags & 16)=0),
    (SELECT count(*)
     FROM things it
     WHERE it.map_id=p.map_id
       AND (it.flags & p.skill_bit)<>0 AND (it.flags & 16)=0
       AND EXISTS (
         SELECT 1 FROM pickup_defs pd
         WHERE pd.thing_type=it.type AND pd.counts_item
       )),
    (SELECT count(*) FROM sectors s
     JOIN sector_special_defs sd ON sd.special=s.special AND sd.is_secret
     WHERE s.map_id=p.map_id)
  FROM params p
  JOIN things t ON t.map_id=p.map_id
  JOIN thing_role_defs r ON r.thing_type=t.type AND r.player_number=1
  JOIN maps m ON m.map_id=t.map_id;
  RETURN 0;
END;;

-- ==== 05_spawn_player.sql ====
DROP FUNCTION IF EXISTS doom_spawn_player;;
CREATE FUNCTION doom_spawn_player(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_view_height DOUBLE;
  SET v_view_height = doom_const('VIEWHEIGHT');
  -- Initialize a player at its immutable WAD Thing start.
  WITH
  spawn AS (
    SELECT t.map_id,t.id AS player_thing_id,
           t.x::float8 AS x,t.y::float8 AS y,
           t.angle::float8 AS angle
    FROM things t
    JOIN thing_role_defs r ON r.thing_type=t.type AND r.is_player_start
    WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int
  ),
  pose AS (
    SELECT p.*,s.floor_height::float8+v_view_height AS view_z,s.id AS sector_id
    FROM spawn p
    JOIN sectors s ON s.map_id=p.map_id
     AND s.id=doom_sector_at(p.map_id,p.x,p.y)
  )
  UPDATE player_state ps
  SET previous_x=p.x,previous_y=p.y,
      position_x=p.x,position_y=p.y,
      base_z=p.view_z,view_z=p.view_z,view_angle=p.angle,
      previous_view_z=p.view_z,previous_view_angle=p.angle,
      momentum_x=0,momentum_y=0,bob_strength=0,
      sector_id=p.sector_id
  FROM pose p
  WHERE ps.map_id=p.map_id AND ps.player_thing_id=p.player_thing_id;
  -- Mirror the initialized SQL pose to the Thing read by rendering and combat.
  UPDATE things t
  SET x=ps.position_x,y=ps.position_y,z=ps.view_z,angle=ps.view_angle
  FROM player_state ps
  WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int
    AND ps.map_id=t.map_id AND ps.player_thing_id=t.id;
  RETURN 0;
END;;

-- ==== 06_finish_level.sql ====
DROP FUNCTION IF EXISTS doom_finish_level;;
CREATE FUNCTION doom_finish_level(a_map_id INT, a_player_thing_id INT, a_secret_exit BOOLEAN) RETURNS INT
BEGIN
  -- Freeze the current run's authoritative SQL state for the intermission.
  WITH params AS (
    SELECT a_map_id::int AS map_id,a_player_thing_id::int AS player_thing_id,
           a_secret_exit::boolean AS secret_exit
  ),
  kill_count AS (
    SELECT count(*)::int AS value
    FROM params p
    JOIN thing_health h ON h.map_id=p.map_id AND NOT h.alive
    JOIN things t ON t.map_id=h.map_id AND t.id=h.thing_id
    JOIN thing_combat_defs d ON d.thing_type=t.type AND d.counts_kill
  ),
  item_count AS (
    SELECT count(*)::int AS value
    FROM params p
    JOIN picked_up_items pi ON pi.map_id=p.map_id
    JOIN things t ON t.map_id=pi.map_id AND t.id=pi.thing_id
    WHERE EXISTS (
      SELECT 1 FROM pickup_defs d
      WHERE d.thing_type=t.type AND d.counts_item
    )
  ),
  secret_count AS (
    SELECT count(*)::int AS value
    FROM params p
    JOIN level_secret_discoveries sd
      ON sd.map_id=p.map_id AND sd.player_thing_id=p.player_thing_id
  )
  UPDATE level_stats ls
  SET level_tics=ps.level_tics,
      kills=k.value,items=i.value,secrets=s.value,
      completed=TRUE,secret_exit=p.secret_exit
  FROM params p,kill_count k,item_count i,secret_count s,player_state ps
  WHERE ls.map_id=p.map_id AND ls.player_thing_id=p.player_thing_id
    AND ps.map_id=p.map_id AND ps.player_thing_id=p.player_thing_id;
  RETURN 0;
END;;

-- ==== 07_cheat_arsenal.sql ====
DROP FUNCTION IF EXISTS doom_cheat_arsenal;;
CREATE FUNCTION doom_cheat_arsenal(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  INSERT INTO player_weapon_owned (map_id,player_thing_id,weapon_id)
  SELECT a_map_id,a_player_thing_id,w.weapon_id
  FROM (VALUES (1),(2),(3),(4),(5),(6),(7),(8)) w(weapon_id)
  ON CONFLICT (map_id,player_thing_id,weapon_id) DO NOTHING;
  UPDATE player_state
  SET ammo_bullets=(SELECT CASE WHEN backpack THEN ad.backpack_cap ELSE ad.cap END
                    FROM ammo_defs ad WHERE ad.ammo_type='bullets'),
      ammo_shells=(SELECT CASE WHEN backpack THEN ad.backpack_cap ELSE ad.cap END
                   FROM ammo_defs ad WHERE ad.ammo_type='shells'),
      ammo_rockets=(SELECT CASE WHEN backpack THEN ad.backpack_cap ELSE ad.cap END
                    FROM ammo_defs ad WHERE ad.ammo_type='rockets'),
      ammo_cells=(SELECT CASE WHEN backpack THEN ad.backpack_cap ELSE ad.cap END
                  FROM ammo_defs ad WHERE ad.ammo_type='cells')
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id;
  RETURN 7;
END;;

-- ==== 08_cs_begin.sql ====
DROP FUNCTION IF EXISTS doom_cs_begin;;
CREATE FUNCTION doom_cs_begin(a_map_id INT, a_player_thing_id INT, a_skill INT, a_move_fwd DOUBLE, a_move_strafe DOUBLE, a_running BOOLEAN, a_turn_degrees DOUBLE, a_attack_held BOOLEAN, a_weapon_switch_to INT, a_use_requested BOOLEAN, a_jump_requested BOOLEAN) RETURNS INT
BEGIN
  -- Clear the previous tic's transient pickup hand-offs.
  DELETE FROM pickup_touches
  WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
  DELETE FROM pickup_grants
  WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
  -- Latch one client command for an authoritative SQL gameplay tic.
  INSERT INTO game_tic_commands
    (map_id,player_thing_id,command_serial,skill,skill_bit,
     move_fwd,move_strafe,running,turn_degrees,attack_held,
     weapon_switch_to,use_requested,movement_mode,jump_requested)
  VALUES
    (a_map_id::int,a_player_thing_id::int,1,a_skill::int,
     -- P_SpawnMapThing's own mapping: sk_baby and sk_easy share MTF_EASY,
     -- sk_medium is MTF_NORMAL, sk_hard and sk_nightmare share MTF_HARD.
     CASE WHEN a_skill::int<=1 THEN 1 WHEN a_skill::int=2 THEN 2 ELSE 4 END,
     a_move_fwd::real,a_move_strafe::real,a_running::boolean,a_turn_degrees::real,a_attack_held::boolean,
     a_weapon_switch_to::int,a_use_requested::boolean,'idle',COALESCE(a_jump_requested,FALSE)::boolean)
  ON CONFLICT (map_id,player_thing_id) DO UPDATE SET
    command_serial=game_tic_commands.command_serial+1,
    skill=EXCLUDED.skill,
    skill_bit=EXCLUDED.skill_bit,
    move_fwd=EXCLUDED.move_fwd,
    move_strafe=EXCLUDED.move_strafe,
    running=EXCLUDED.running,
    turn_degrees=EXCLUDED.turn_degrees,
    attack_held=EXCLUDED.attack_held,
    weapon_switch_to=EXCLUDED.weapon_switch_to,
    use_requested=EXCLUDED.use_requested,
    jump_requested=EXCLUDED.jump_requested,
    movement_mode='idle';
  RETURN 0;
END;;

-- ==== 09_cs_clock.sql ====
DROP FUNCTION IF EXISTS doom_cs_clock;;
CREATE FUNCTION doom_cs_clock(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- One authoritative 35 Hz clock step
  UPDATE player_state
  SET level_tics=level_tics+1,
      pain_face_tics=GREATEST(0,pain_face_tics-1),
      radsuit_tics=GREATEST(0,radsuit_tics-1),
      invis_tics=GREATEST(0,invis_tics-1),
      light_amp_tics=GREATEST(0,light_amp_tics-1),
      invuln_tics=GREATEST(0,invuln_tics-1),
      message_tics=GREATEST(0,message_tics-1),
      message=CASE WHEN message_tics<=1 THEN NULL ELSE message END,
      damage_count=GREATEST(0,damage_count-1),
      bonus_count=GREATEST(0,bonus_count-1)
  WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
  RETURN 0;
END;;

-- ==== 10_cs_plan.sql ====
DROP FUNCTION IF EXISTS doom_cs_plan;;
CREATE FUNCTION doom_cs_plan(a_map_id INT, a_player_thing_id INT) RETURNS BIGINT
BEGIN
  DECLARE v_flags BIGINT;
  DECLARE v_pos_epsilon DOUBLE;
  DECLARE v_psprite_epsilon DOUBLE;
  DECLARE v_psprite_rest_x DOUBLE;
  DECLARE v_psprite_rest_y DOUBLE;
  DECLARE v_use_requested BOOLEAN;
  DECLARE v_specials_due BOOLEAN;
  DECLARE v_movers_due BOOLEAN;
  DECLARE v_movement_mode TEXT;
  DECLARE v_moved BOOLEAN;
  DECLARE v_weapon_due BOOLEAN;
  DECLARE v_hitscan_due BOOLEAN;
  DECLARE v_projectiles_due BOOLEAN;
  DECLARE v_sound_due BOOLEAN;
  SET v_flags = 0::bigint;
  SET v_pos_epsilon = doom_const('POS_EPSILON');
  SET v_psprite_epsilon = doom_const('PSPRITE_EPSILON');
  SET v_psprite_rest_x = doom_const('PSPRITE_REST_X');
  SET v_psprite_rest_y = doom_const('PSPRITE_REST_Y');
  -- SQL-generated execution plan for the current tic.
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR WITH input AS (
        SELECT g.*,ps.previous_x,ps.previous_y,ps.position_x,ps.position_y,
               ps.bob_strength,ps.alive,ps.pain_face_tics,
               pw.state AS weapon_state,pw.flash_seq_index,pw.fired_this_tick,
               pw.sx AS weapon_sx,pw.sy AS weapon_sy,
               pw.current_weapon
        FROM game_tic_commands g
        JOIN player_state ps ON ps.map_id=g.map_id
          AND ps.player_thing_id=g.player_thing_id
        JOIN player_weapons pw ON pw.map_id=g.map_id
          AND pw.player_thing_id=g.player_thing_id
        WHERE g.map_id=a_map_id::int AND g.player_thing_id=a_player_thing_id::int
      )
      SELECT
        i.use_requested,
        -- Any queued event on the map by any actor?
        EXISTS (
          SELECT 1 FROM line_special_events e WHERE e.map_id=i.map_id
        ) AS specials_due,
        (EXISTS (SELECT 1 FROM sector_movers sm
                 WHERE sm.map_id=i.map_id AND sm.direction<>2)
         OR EXISTS (SELECT 1 FROM line_buttons b
                    WHERE b.map_id=i.map_id AND b.countdown>0)) AS movers_due,
        i.movement_mode,
        (ABS(i.position_x-i.previous_x)>v_pos_epsilon
         OR ABS(i.position_y-i.previous_y)>v_pos_epsilon) AS moved,
        (i.attack_held OR i.weapon_switch_to IS NOT NULL
         OR EXISTS (SELECT 1 FROM pickup_grants pg
                    WHERE pg.map_id=i.map_id
                      AND pg.player_thing_id=i.player_thing_id)
         OR i.weapon_state<>'ready' OR i.flash_seq_index IS NOT NULL
         OR i.bob_strength>v_psprite_epsilon
         OR ABS(i.weapon_sx-v_psprite_rest_x)>v_psprite_epsilon
         OR ABS(i.weapon_sy-v_psprite_rest_y)>v_psprite_epsilon
         OR EXISTS (SELECT 1 FROM world_effects we WHERE we.map_id=i.map_id))
          AS weapon_due,
        (i.fired_this_tick AND wd.pellet_count>0) AS hitscan_due,
        (EXISTS (SELECT 1 FROM monster_projectiles mp WHERE mp.map_id=i.map_id)
         OR (i.fired_this_tick AND wd.projectile_type IS NOT NULL)
         OR EXISTS (
           SELECT 1 FROM monster_ai ai
           JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
           JOIN thing_combat_defs d ON d.thing_type=t.type
           WHERE ai.map_id=i.map_id AND ai.fired_this_tick AND d.missile_type IS NOT NULL
         )) AS projectiles_due,
        (i.use_requested OR i.fired_this_tick
         OR EXISTS (SELECT 1 FROM pickup_touches pt
                    WHERE pt.map_id=i.map_id
                      AND pt.player_thing_id=i.player_thing_id)
         OR EXISTS (SELECT 1 FROM monster_ai ai
                    WHERE ai.map_id=i.map_id AND ai.fired_this_tick)
         OR EXISTS (SELECT 1 FROM projectile_impacts pi WHERE pi.map_id=i.map_id)
         OR EXISTS (SELECT 1 FROM sector_movers sm
                    WHERE sm.map_id=i.map_id AND sm.direction IN (-1,1))
         OR (wd.idle_loop_sound IS NOT NULL AND i.weapon_state<>'down')
         OR EXISTS (SELECT 1 FROM monster_ai ai
                    WHERE ai.map_id=i.map_id AND ai.state <> 'stand')
         OR NOT i.alive
         OR i.pain_face_tics > 0) AS sound_due
      FROM input i
      JOIN weapon_defs wd ON wd.weapon_id=i.current_weapon;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_use_requested, v_specials_due, v_movers_due, v_movement_mode, v_moved, v_weapon_due, v_hitscan_due, v_projectiles_due, v_sound_due;
      IF b1_done THEN LEAVE b1_each; END IF;
      IF v_use_requested THEN
        SET v_flags = v_flags + 1;
      END IF;
      IF v_specials_due THEN
        SET v_flags = v_flags + 2;
      END IF;
      IF v_movers_due THEN
        SET v_flags = v_flags + 4;
      END IF;
      IF v_moved THEN
        SET v_flags = v_flags + 8;
      END IF;
      IF v_weapon_due THEN
        SET v_flags = v_flags + 16;
      END IF;
      IF v_hitscan_due THEN
        SET v_flags = v_flags + 32;
      END IF;
      IF v_projectiles_due THEN
        SET v_flags = v_flags + 64;
      END IF;
      IF v_sound_due THEN
        SET v_flags = v_flags + 128;
      END IF;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_flags;
END;;

-- ==== 11_cs_use.sql ====
DROP FUNCTION IF EXISTS doom_cs_use;;
CREATE FUNCTION doom_cs_use(a_map_id INT, a_player_thing_id INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_use_range DOUBLE;
  DECLARE v_result_queued BOOLEAN;
  DECLARE v_queued BOOLEAN;
  DECLARE v_line_id BIGINT;
  DECLARE v_locked BOOLEAN;
  DECLARE v_required_key TEXT;
  DECLARE v_is_exit BOOLEAN;
  DECLARE v_exit_secret BOOLEAN;
  SET v_use_range = doom_const('USERANGE');
  -- Trace the first usable/blocking line within USERANGE and queue a supported
  -- use-special for the special-activation function.
  DELETE FROM line_use_results
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id;
  WITH
  params AS (
    SELECT g.map_id,g.player_thing_id,
           COALESCE(ps.position_x, t.x)::float8 AS px,
           COALESCE(ps.position_y, t.y)::float8 AS py,
           COS(RADIANS(COALESCE(ps.view_angle, t.angle)::float8)) AS dx,
           SIN(RADIANS(COALESCE(ps.view_angle, t.angle)::float8)) AS dy,
           GREATEST(v_use_range, 250.0::double precision) AS use_range
    FROM game_tic_commands g
    JOIN things t ON t.map_id=g.map_id AND t.id=g.player_thing_id
    LEFT JOIN player_state ps ON ps.map_id=g.map_id AND ps.player_thing_id=g.player_thing_id
    WHERE g.map_id=a_map_id::int AND g.player_thing_id=a_player_thing_id::int
      AND g.use_requested
  ),
  line_geom AS (
    SELECT ld.linedef_id AS id,ld.special,ld.flags,ld.left_sd_id,ld.right_sd_id,
           ld.x1::float8 AS x1,ld.y1::float8 AS y1,
           ld.x2::float8 AS x2,ld.y2::float8 AS y2,
           rf.floor_height AS front_floor,rf.ceil_height AS front_ceil,
           rb.floor_height AS back_floor,rb.ceil_height AS back_ceil
    FROM params p
    JOIN linedef_geom ld ON ld.map_id=p.map_id
    LEFT JOIN sectors rf ON rf.map_id=ld.map_id AND rf.id=ld.fsec
    LEFT JOIN sectors rb ON rb.map_id=ld.map_id AND rb.id=ld.bsec
  ),
  intersections AS (
    SELECT l.*,
      ((l.x1-p.px)*(l.y2-l.y1)-(l.y1-p.py)*(l.x2-l.x1))/d.denom AS distance,
      ((l.x1-p.px)*p.dy-(l.y1-p.py)*p.dx)/d.denom AS line_fraction,
      ((l.x2-l.x1)*(p.py-l.y1)-(l.y2-l.y1)*(p.px-l.x1)) < 0 AS from_front
    FROM line_geom l CROSS JOIN params p
    CROSS JOIN LATERAL (
      SELECT p.dx*(l.y2-l.y1)-p.dy*(l.x2-l.x1) AS denom
    ) d
    WHERE ABS(d.denom)>1e-9
  ),
  trace_candidates AS (
    SELECT i.* FROM intersections i CROSS JOIN params p
    WHERE i.distance BETWEEN 0 AND p.use_range
      AND i.line_fraction BETWEEN -0.05 AND 1.05
      AND (i.special<>0 OR i.left_sd_id=-1 OR i.right_sd_id=-1
        OR (i.flags&1)<>0
        OR (LEAST(i.front_ceil,i.back_ceil)
          - GREATEST(i.front_floor,i.back_floor))<=0)
  ),
  hit AS (
    SELECT * FROM trace_candidates
    ORDER BY (CASE WHEN special <> 0 THEN 0 ELSE 1 END), distance, id
    LIMIT 1
  ),
  decision AS (
    SELECT h.*,
      COALESCE((d.key_required='red'    AND NOT ps.key_red)
            OR (d.key_required='blue'   AND NOT ps.key_blue)
            OR (d.key_required='yellow' AND NOT ps.key_yellow),FALSE) AS locked,
      (COALESCE(d.use_activated,FALSE) AND (d.mechanic='door' OR h.from_front)) AS supported,
      COALESCE(d.use_once,FALSE) AS one_shot
    FROM hit h CROSS JOIN params p
    LEFT JOIN line_special_defs d ON d.special=h.special
    JOIN player_state ps ON ps.map_id=p.map_id
      AND ps.player_thing_id=p.player_thing_id
  )
  INSERT INTO line_use_results
    (map_id,player_thing_id,line_id,special,locked,eligible,from_front)
  SELECT p.map_id,p.player_thing_id,d.id,d.special,d.locked,
         d.supported AND NOT d.locked
           AND NOT (d.one_shot AND EXISTS (
             SELECT 1 FROM line_activations a
             WHERE a.map_id=p.map_id AND a.line_id=d.id
           )),
         d.from_front
  FROM decision d CROSS JOIN params p
  ON CONFLICT (map_id,player_thing_id) DO UPDATE SET
    line_id=EXCLUDED.line_id,
    special=EXCLUDED.special,
    locked=EXCLUDED.locked,
    eligible=EXCLUDED.eligible,
    from_front=EXCLUDED.from_front;
  -- Queue the staged decision.
  INSERT INTO line_special_events
    (map_id,player_thing_id,line_id,trigger_type,from_front)
  SELECT map_id,player_thing_id,line_id,'use',from_front
  FROM line_use_results
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id AND eligible
  ON CONFLICT (map_id,player_thing_id,line_id,trigger_type) DO NOTHING;
  SET v_result_queued = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(r.eligible,FALSE) AS queued,
             r.line_id,
             COALESCE(r.locked,FALSE) AS locked,
             CASE WHEN COALESCE(r.locked,FALSE) THEN d.key_required::text END
               AS required_key,
             COALESCE(r.eligible AND d.is_exit,FALSE) AS is_exit,
             COALESCE(r.eligible AND d.secret_exit,FALSE) AS exit_secret
      FROM (SELECT a_map_id::int AS map_id, a_player_thing_id::int AS player_thing_id) p
      LEFT JOIN line_use_results r ON r.map_id=p.map_id
        AND r.player_thing_id=p.player_thing_id
      LEFT JOIN line_special_defs d ON d.special=r.special;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_queued, v_line_id, v_locked, v_required_key, v_is_exit, v_exit_secret;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_result_queued = v_queued;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_result_queued;
END;;

-- ==== 12_cs_activate_specials.sql ====
DROP FUNCTION IF EXISTS doom_cs_activate_specials;;
CREATE FUNCTION doom_cs_activate_specials(a_map_id INT) RETURNS BIGINT
BEGIN
  DECLARE v_flags BIGINT;
  DECLARE v_stale_due BOOLEAN;
  DECLARE v_doors_due BOOLEAN;
  DECLARE v_floors_due BOOLEAN;
  DECLARE v_lifts_due BOOLEAN;
  DECLARE v_donuts_due BOOLEAN;
  DECLARE v_buttons_due BOOLEAN;
  DECLARE v_raises_due BOOLEAN;
  DECLARE v_stairs_due BOOLEAN;
  DECLARE v_lights_due BOOLEAN;
  DECLARE v_teleport_due BOOLEAN;
  DECLARE v_ceilings_due BOOLEAN;
  DECLARE v_crushers_due BOOLEAN;
  DECLARE v_stops_due BOOLEAN;
  DECLARE v_activation_due BOOLEAN;
  DECLARE v_active BIGINT;
  DECLARE v_active_movers BOOLEAN;
  DECLARE v_active_buttons BOOLEAN;
  SET v_flags = 0::bigint;
  DELETE FROM line_special_events e
  USING linedefs ld, line_special_defs d
  WHERE e.map_id=a_map_id AND ld.map_id=e.map_id AND ld.id=e.line_id AND d.special=ld.special
    AND EXISTS (
      SELECT 1 FROM line_special_events e2
      JOIN linedefs ld2 ON ld2.map_id=e2.map_id AND ld2.id=e2.line_id
      JOIN line_special_defs d2 ON d2.special=ld2.special
      WHERE e2.map_id=e.map_id AND d2.mechanic=d.mechanic
        AND ((ld.tag<>0 AND ld2.tag=ld.tag) OR (ld.tag=0 AND ld2.id=ld.id))
        AND (ld2.id<ld.id OR (ld2.id=ld.id AND e2.player_thing_id<e.player_thing_id)));
  -- Dispatch plan for queued linedef specials
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR WITH events AS (
        SELECT e.map_id,e.line_id,ld.special,ld.right_sd_id,d.mechanic,d.flips_switch,
               (COALESCE(d.cross_once,FALSE) OR COALESCE(d.use_once,FALSE)) AS one_shot,
               (COALESCE(d.cross_once,FALSE) OR COALESCE(d.use_once,FALSE))
                AND EXISTS (
                  SELECT 1 FROM line_activations a
                  WHERE a.map_id=e.map_id AND a.line_id=e.line_id
                ) AS stale,
               (COALESCE(sd.upper_tex,'') LIKE 'SW1%'
                OR COALESCE(sd.mid_tex,'') LIKE 'SW1%'
                OR COALESCE(sd.lower_tex,'') LIKE 'SW1%') AS has_switch_texture
        FROM line_special_events e
        JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
        LEFT JOIN line_special_defs d ON d.special=ld.special
        LEFT JOIN sidedefs sd ON sd.map_id=ld.map_id AND sd.id=ld.right_sd_id
        WHERE e.map_id=a_map_id::int
      )
      SELECT
        COALESCE(BOOL_OR(stale),FALSE) AS stale_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='door'),FALSE) AS doors_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='floor'),FALSE) AS floors_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='platform'),FALSE) AS lifts_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='donut'),FALSE) AS donuts_due,
        COALESCE(BOOL_OR(NOT stale AND has_switch_texture AND flips_switch),FALSE) AS buttons_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='raise'),FALSE) AS raises_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='stairs'),FALSE) AS stairs_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='lights'),FALSE) AS lights_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='teleport'),FALSE) AS teleport_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='ceiling'),FALSE) AS ceilings_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='crusher'),FALSE) AS crushers_due,
        COALESCE(BOOL_OR(NOT stale AND mechanic='stop'),FALSE) AS stops_due,
        COALESCE(BOOL_OR(NOT stale AND one_shot),FALSE) AS activation_due
      FROM events;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_stale_due, v_doors_due, v_floors_due, v_lifts_due, v_donuts_due, v_buttons_due, v_raises_due, v_stairs_due, v_lights_due, v_teleport_due, v_ceilings_due, v_crushers_due, v_stops_due, v_activation_due;
      IF b1_done THEN LEAVE b1_each; END IF;
      IF v_stale_due THEN
        SET v_flags = v_flags + 1;
      END IF;
      IF v_doors_due THEN
        SET v_flags = v_flags + 2;
      END IF;
      IF v_floors_due THEN
        SET v_flags = v_flags + 4;
      END IF;
      IF v_lifts_due THEN
        SET v_flags = v_flags + 8;
      END IF;
      IF v_donuts_due THEN
        SET v_flags = v_flags + 16;
      END IF;
      IF v_buttons_due THEN
        SET v_flags = v_flags + 32;
      END IF;
      IF v_activation_due THEN
        SET v_flags = v_flags + 64;
      END IF;
      IF v_raises_due THEN
        SET v_flags = v_flags + 128;
      END IF;
      IF v_stairs_due THEN
        SET v_flags = v_flags + 256;
      END IF;
      IF v_lights_due THEN
        SET v_flags = v_flags + 512;
      END IF;
      IF v_teleport_due THEN
        SET v_flags = v_flags + 1024;
      END IF;
      IF v_ceilings_due THEN
        SET v_flags = v_flags + 2048;
      END IF;
      IF v_crushers_due THEN
        SET v_flags = v_flags + 4096;
      END IF;
      IF v_stops_due THEN
        SET v_flags = v_flags + 8192;
      END IF;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF (v_flags & 1) <> 0 THEN
    -- Consume queued use/cross/shoot linedef events and instantiate Doom-style
    -- tagged sector movers.
    -- The script is executed as separately prepared statements on an autocommit
    -- connection.  If execution is interrupted after recording a one-shot
    -- activation but before deleting its queued event, the event survives while
    -- the activation marker is already committed.  Drop such stale events before
    -- dispatching anything: replaying their effects is wrong, and attempting to
    -- record the same marker again produces a duplicate-primary-key error on
    -- CedarDB even with ON CONFLICT DO NOTHING.
    DELETE FROM line_special_events e
    WHERE e.map_id=a_map_id AND EXISTS (
      SELECT 1
      FROM linedefs ld JOIN line_activations a
        ON a.map_id=ld.map_id AND a.line_id=ld.id
      JOIN line_special_defs d ON d.special=ld.special
        AND (d.cross_once OR d.use_once)
      WHERE ld.map_id=e.map_id AND ld.id=e.line_id
    );
  END IF;
  IF (v_flags & 2) <> 0 THEN
    -- Doors: manual back-sector doors plus tagged remote doors.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,e.trigger_type,e.from_front,ld.special,ld.tag,ld.left_sd_id,ld.right_sd_id
      FROM line_special_events e
      JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
      WHERE e.map_id=a_map_id
    ),
    door_sectors AS (
      -- 'back' is the manual door you open by walking into it, which moves the
      -- sector behind the line; 'tag' is a remote door, which moves every sector
      -- carrying the line's tag.
      SELECT e.*,
        CASE WHEN d.door_target='back' THEN
          COALESCE(CASE WHEN e.from_front THEN back.sector_id ELSE front.sector_id END, back.sector_id, front.sector_id)
        ELSE s.id END AS sector_id
      FROM events e
      JOIN line_special_defs d ON d.special=e.special AND d.mechanic='door'
      LEFT JOIN sidedefs back ON back.map_id=e.map_id AND back.id=e.left_sd_id
      LEFT JOIN sidedefs front ON front.map_id=e.map_id AND front.id=e.right_sd_id
      LEFT JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      WHERE (d.door_target='back' AND (back.sector_id IS NOT NULL OR front.sector_id IS NOT NULL))
         OR (d.door_target='tag'  AND s.id IS NOT NULL)
    ),
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    neighbors AS (
      SELECT d.map_id,d.line_id,d.special,d.sector_id,other.ceil_height
      FROM door_sectors d
      JOIN adjacency a ON a.sector_id=d.sector_id
      JOIN sectors other ON other.map_id=d.map_id AND other.id=a.other_id
    ),
    targets AS (
      SELECT d.map_id,d.line_id,d.special,d.sector_id,s.floor_height,s.ceil_height,
        GREATEST(s.ceil_height,COALESCE(MIN(n.ceil_height)-4,s.ceil_height))::smallint
          AS open_height
      FROM door_sectors d
      JOIN sectors s ON s.map_id=d.map_id AND s.id=d.sector_id
      LEFT JOIN neighbors n ON n.map_id=d.map_id AND n.line_id=d.line_id
        AND n.sector_id=d.sector_id
      GROUP BY d.map_id,d.line_id,d.special,d.sector_id,s.floor_height,s.ceil_height
    ),
    targets_unique AS (
      SELECT * FROM (
        SELECT t.*,ROW_NUMBER() OVER
          (PARTITION BY t.map_id,t.sector_id ORDER BY t.line_id) AS rn
        FROM targets t) q
      WHERE rn=1
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT t.map_id,t.sector_id,t.line_id,
      d.mover_type,
      'ceiling',
      d.direction,
      t.floor_height,
      -- A closing door's travel ends back at the ceiling it started from;
      -- everything else opens to just under the lowest neighbouring ceiling.
      CASE WHEN d.direction=-1 THEN t.ceil_height ELSE t.open_height END,
      d.speed,
      d.wait_tics,
      0,NULL,NULL,NULL,FALSE
    FROM targets_unique t
    JOIN line_special_defs d ON d.special=t.special
    ON CONFLICT (map_id,sector_id) DO UPDATE SET
      -- A completed manual raiser (direction=2) is sitting closed, so a fresh
      -- use must start it opening again. Active doors retain Doom's use-toggle:
      -- closing reverses upward; opening/waiting starts closing. Only a manual
      -- door does this; any other mover in the sector is left alone.
      source_line_id=CASE WHEN EXCLUDED.mover_type='door_manual_raise' AND sector_movers.plane='ceiling'
                          THEN EXCLUDED.source_line_id ELSE sector_movers.source_line_id END,
      direction=CASE WHEN EXCLUDED.mover_type='door_manual_raise' AND sector_movers.plane='ceiling'
                     THEN CASE WHEN sector_movers.direction IN (-1,2) THEN 1 ELSE -1 END
                     ELSE sector_movers.direction END,
      countdown=CASE WHEN EXCLUDED.mover_type='door_manual_raise' AND sector_movers.plane='ceiling'
                     THEN 0 ELSE sector_movers.countdown END,
      next_ceiling=CASE WHEN EXCLUDED.mover_type='door_manual_raise' AND sector_movers.plane='ceiling'
                        THEN NULL ELSE sector_movers.next_ceiling END,
      moved_this_tick=CASE WHEN EXCLUDED.mover_type='door_manual_raise' AND sector_movers.plane='ceiling'
                           THEN FALSE ELSE sector_movers.moved_this_tick END;
  END IF;
  IF (v_flags & 4) <> 0 THEN
    -- Floor lowering.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.special,ld.tag,
             d.height_target,d.mover_type,d.direction,d.speed,d.change_tex
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='floor'
      WHERE e.map_id=a_map_id
    ),
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    targets AS (
      SELECT e.map_id,e.line_id,e.special,e.height_target,e.mover_type,
        e.direction,e.speed,e.change_tex,s.id AS sector_id,s.floor_height,
        MIN(other.floor_height) AS lowest_floor,
        MAX(other.floor_height) AS highest_floor
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      JOIN adjacency a ON a.sector_id=s.id
      JOIN sectors other ON other.map_id=s.map_id AND other.id=a.other_id
      GROUP BY e.map_id,e.line_id,e.special,e.height_target,e.mover_type,
        e.direction,e.speed,e.change_tex,s.id,s.floor_height
    ),
    dest AS (
      -- 'lowest' drops to the lowest neighbour, 'highest' to the highest one,
      -- 'highest8' is the turbo variant that stops 8 above the highest one.
      SELECT *,CASE WHEN height_target='lowest' THEN lowest_floor
        WHEN height_target='highest' THEN highest_floor
        WHEN highest_floor<>floor_height THEN highest_floor+8
        ELSE floor_height END::smallint AS destination
      FROM targets
    ),
    model AS (
      SELECT d.*,
        CASE WHEN d.change_tex THEN
          (SELECT o.floor_tex FROM adjacency a
            JOIN sectors o ON o.map_id=d.map_id AND o.id=a.other_id
            WHERE a.sector_id=d.sector_id AND o.floor_height=d.destination
            ORDER BY o.id LIMIT 1) END AS model_tex
      FROM dest d
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT map_id,sector_id,line_id,mover_type,'floor',direction,
      LEAST(floor_height,destination),floor_height,
      speed,0,0,NULL,NULL,model_tex,FALSE
    FROM model WHERE destination<floor_height
    ON CONFLICT (map_id,sector_id) DO NOTHING;
  END IF;
  IF (v_flags & 8) <> 0 THEN
    -- Down-wait-up lifts.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.special,ld.tag,
             d.mover_type,d.direction,d.speed,d.wait_tics
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='platform'
      WHERE e.map_id=a_map_id
    ),
    -- P_FindLowestFloorSurrounding needs the sectors on the far side of the
    -- tagged sector's own two-sided lines. Deriving that as "join every linedef
    -- on the map, then keep the ones whose sidedefs happen to mention this
    -- sector" has no join condition at all: the real test sits in the WHERE as an
    -- OR across two LEFT-joined tables, which no hash join can use, so it
    -- degenerates into a nested loop over every line for every tagged sector.
    -- One lift activation on E1M2 cost 219 ms because of it.
    --
    -- Equi-joining the sidedef ids instead gives an edge list the planner can
    -- actually index, and says what it means: a two-sided line is an edge between
    -- the two sectors its sidedefs name.
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    targets AS (
      SELECT e.map_id,e.line_id,e.mover_type,e.direction,e.speed,e.wait_tics,
        s.id AS sector_id,s.floor_height,
        MIN(other.floor_height)::smallint AS lowest_floor,
        MAX(other.floor_height)::smallint AS highest_floor
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      JOIN adjacency a ON a.sector_id=s.id
      JOIN sectors other ON other.map_id=s.map_id AND other.id=a.other_id
      GROUP BY e.map_id,e.line_id,e.mover_type,e.direction,e.speed,e.wait_tics,
        s.id,s.floor_height
    ),
    targets_unique AS (
      SELECT * FROM (
        SELECT t.*,ROW_NUMBER() OVER
          (PARTITION BY t.map_id,t.sector_id ORDER BY t.line_id) AS rn
        FROM targets t) q
      WHERE rn=1
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT map_id,sector_id,line_id,mover_type,'floor',direction,
      LEAST(floor_height,lowest_floor),
      -- A perpetual platform runs between the lowest and highest neighbours for
      -- ever (until a stop line); an ordinary lift comes back to where it was.
      CASE WHEN mover_type='platform_perpetual' THEN GREATEST(floor_height,highest_floor)
           ELSE floor_height END,
      speed,wait_tics,0,NULL,NULL,NULL,FALSE
    FROM targets_unique
    ON CONFLICT (map_id,sector_id) DO UPDATE SET
      source_line_id=EXCLUDED.source_line_id,direction=-1,countdown=0,
      next_floor=NULL,moved_this_tick=FALSE
    WHERE sector_movers.direction=2 AND sector_movers.plane='floor';
  END IF;
  IF (v_flags & 16) <> 0 THEN
    -- S1 donut: lower the tagged pillar and raise its surrounding ring to the
    -- next outside sector's floor, adopting that sector's flat at completion.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.tag
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='donut'
      WHERE e.map_id=a_map_id
    ),
    pillar AS (
      SELECT e.map_id,e.line_id,s.id AS pillar_id,s.floor_height AS pillar_floor
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
    ),
    edges AS (
      SELECT sector_id, other_id, linedef_id AS edge_id
      FROM sector_adjacency WHERE map_id=a_map_id
    ),
    ring_candidates AS (
      SELECT p.*, e.other_id AS ring_id, e.edge_id
      FROM pillar p
      JOIN edges e ON e.sector_id=p.pillar_id
    ),
    ring AS (
      SELECT * FROM (SELECT *,ROW_NUMBER() OVER
        (PARTITION BY map_id,line_id,pillar_id ORDER BY edge_id) AS rn
        FROM ring_candidates) q WHERE rn=1
    ),
    outside_candidates AS (
      SELECT r.*, e.other_id AS outside_id, e.edge_id AS outside_edge_id
      FROM ring r
      JOIN edges e ON e.sector_id=r.ring_id
      WHERE e.other_id <> r.pillar_id
    ),
    outside AS (
      SELECT * FROM (SELECT *,ROW_NUMBER() OVER
        (PARTITION BY map_id,line_id,pillar_id ORDER BY outside_edge_id) AS outside_rn
        FROM outside_candidates) q WHERE outside_rn=1
    ),
    motions AS (
      SELECT o.map_id,o.line_id,o.pillar_id AS sector_id,'floor_lower'::text AS mover_type,
        -1::smallint AS direction,LEAST(o.pillar_floor,s.floor_height)::smallint AS bottom_height,
        o.pillar_floor::smallint AS top_height,NULL::varchar(8) AS target_tex
      FROM outside o JOIN sectors s ON s.map_id=o.map_id AND s.id=o.outside_id
      UNION ALL
      SELECT o.map_id,o.line_id,o.ring_id,'donut_raise',1,
        ring.floor_height::smallint,GREATEST(ring.floor_height,s.floor_height)::smallint,
        s.floor_tex
      FROM outside o JOIN sectors ring ON ring.map_id=o.map_id AND ring.id=o.ring_id
      JOIN sectors s ON s.map_id=o.map_id AND s.id=o.outside_id
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT map_id,sector_id,line_id,mover_type,'floor',direction,
      bottom_height,top_height,1,0,0,NULL,NULL,target_tex,FALSE
    FROM motions WHERE bottom_height<>top_height
    ON CONFLICT (map_id,sector_id) DO NOTHING;
  END IF;
  IF (v_flags & 32) <> 0 THEN
    -- Remember visible SW1* texture substitutions. Repeatable buttons restore
    -- after one second; S1/W1 switches stay changed until the stage resets.
    -- A restored repeatable button keeps its row at countdown=0; remove that
    -- stale row before reactivation.
    DELETE FROM line_buttons b WHERE b.map_id=a_map_id AND b.countdown=0 AND EXISTS (
      SELECT 1 FROM line_special_events e
      WHERE e.map_id=b.map_id AND e.line_id=b.line_id
    );
    WITH
    events AS (
      SELECT DISTINCT e.map_id,e.line_id,ld.special,ld.right_sd_id,
        (d.cross_once OR d.use_once) AS one_shot
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.flips_switch
      WHERE e.map_id=a_map_id
    ),
    parts AS (
      SELECT e.map_id,e.line_id,e.right_sd_id AS sidedef_id,'upper'::text AS texture_part,
        sd.upper_tex AS tex,
        -- A repeatable switch springs back after a second; a one-shot one
        -- stays pressed (-1).
        CASE WHEN e.one_shot THEN -1 ELSE 35 END AS countdown
      FROM events e JOIN sidedefs sd ON sd.map_id=e.map_id AND sd.id=e.right_sd_id
      WHERE sd.upper_tex LIKE 'SW1%'
      UNION ALL
      SELECT e.map_id,e.line_id,e.right_sd_id,'middle',sd.mid_tex,
        CASE WHEN e.one_shot THEN -1 ELSE 35 END
      FROM events e JOIN sidedefs sd ON sd.map_id=e.map_id AND sd.id=e.right_sd_id
      WHERE sd.mid_tex LIKE 'SW1%'
      UNION ALL
      SELECT e.map_id,e.line_id,e.right_sd_id,'lower',sd.lower_tex,
        CASE WHEN e.one_shot THEN -1 ELSE 35 END
      FROM events e JOIN sidedefs sd ON sd.map_id=e.map_id AND sd.id=e.right_sd_id
      WHERE sd.lower_tex LIKE 'SW1%'
    )
    INSERT INTO line_buttons
      (map_id,line_id,sidedef_id,texture_part,original_tex,active_tex,countdown,
       just_restored)
    SELECT map_id,line_id,sidedef_id,texture_part,tex,
      ('SW2'||SUBSTRING(tex FROM 4))::varchar(8),countdown,FALSE
    FROM parts
    ON CONFLICT (map_id,line_id,sidedef_id,texture_part) DO UPDATE SET
      countdown=EXCLUDED.countdown;
    WITH changed AS (
      SELECT DISTINCT b.map_id,b.sidedef_id
      FROM line_buttons b
      WHERE b.map_id=a_map_id AND EXISTS (
        SELECT 1 FROM line_special_events e
        WHERE e.map_id=b.map_id AND e.line_id=b.line_id
      )
    )
    UPDATE sidedefs sd
    SET upper_tex=COALESCE(u.active_tex,sd.upper_tex),
        mid_tex=COALESCE(m.active_tex,sd.mid_tex),
        lower_tex=COALESCE(l.active_tex,sd.lower_tex)
    FROM changed c
    LEFT JOIN line_buttons u ON u.map_id=c.map_id AND u.sidedef_id=c.sidedef_id
      AND u.texture_part='upper'
    LEFT JOIN line_buttons m ON m.map_id=c.map_id AND m.sidedef_id=c.sidedef_id
      AND m.texture_part='middle'
    LEFT JOIN line_buttons l ON l.map_id=c.map_id AND l.sidedef_id=c.sidedef_id
      AND l.texture_part='lower'
    WHERE sd.map_id=c.map_id AND sd.id=c.sidedef_id;
    UPDATE render_segs rs
    SET upper_tex=sd.upper_tex,mid_tex=sd.mid_tex,lower_tex=sd.lower_tex
    FROM linedefs ld,sidedefs sd
    WHERE rs.map_id=a_map_id AND ld.map_id=rs.map_id AND ld.id=rs.linedef_id
      AND sd.map_id=ld.map_id
      AND sd.id=CASE WHEN rs.direction=0 THEN ld.right_sd_id ELSE ld.left_sd_id END;
  END IF;
  IF (v_flags & 64) <> 0 THEN
    -- Consume one-shot lines now, after their valid key/side/trigger event was
    -- queued.
    INSERT INTO line_activations (map_id,line_id)
    SELECT DISTINCT e.map_id,e.line_id
    FROM line_special_events e JOIN linedefs ld
      ON ld.map_id=e.map_id AND ld.id=e.line_id
    JOIN line_special_defs d ON d.special=ld.special
      AND (d.cross_once OR d.use_once)
    WHERE e.map_id=a_map_id
    ON CONFLICT (map_id, line_id) DO NOTHING;
  END IF;
  IF (v_flags & 128) <> 0 THEN
    -- Floor RAISES. height_target says where it stops: 'lowest_ceiling' is the
    -- lowest surrounding ceiling, 'next_floor' is the next floor above this on.
    -- The change_tex variants additionally adopt that model sector's flat
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.special,ld.tag,
             d.height_target,d.change_tex,d.mover_type,d.direction,d.speed,d.crush,
             fs.floor_tex AS front_tex
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='raise'
      LEFT JOIN sidedefs fsd ON fsd.map_id=ld.map_id AND fsd.id=ld.right_sd_id
      LEFT JOIN sectors fs ON fs.map_id=ld.map_id AND fs.id=fsd.sector_id
      WHERE e.map_id=a_map_id
    ),
    -- The shortest lower texture on the sector's two-sided lines, either side.
    shortest AS (
      SELECT sec.id AS sector_id, MIN(wt.height) AS shortest
      FROM sectors sec
      JOIN sidedefs sd ON sd.map_id=sec.map_id AND sd.sector_id=sec.id
      JOIN linedefs ld ON ld.map_id=sd.map_id AND (ld.right_sd_id=sd.id OR ld.left_sd_id=sd.id)
        AND ld.right_sd_id IS NOT NULL AND ld.left_sd_id IS NOT NULL
      JOIN sidedefs both_sides ON both_sides.map_id=ld.map_id
        AND both_sides.id IN (ld.right_sd_id, ld.left_sd_id)
      JOIN walltex_meta wt ON wt.name=both_sides.lower_tex
      WHERE sec.map_id=a_map_id AND both_sides.lower_tex IS NOT NULL AND both_sides.lower_tex<>'-'
      GROUP BY sec.id
    ),
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    neighbors AS (
      SELECT e.map_id,e.line_id,e.special,e.height_target,e.change_tex,
             e.mover_type,e.direction,e.speed,e.crush,e.front_tex,
             s.id AS sector_id,s.floor_height,s.ceil_height,
             other.id AS other_id,other.floor_height AS other_floor,
             other.ceil_height AS other_ceil,other.floor_tex AS other_tex
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      JOIN adjacency a ON a.sector_id=s.id
      JOIN sectors other ON other.map_id=s.map_id AND other.id=a.other_id
    ),
    targets AS (
      SELECT n.map_id,n.line_id,n.special,n.height_target,n.change_tex,n.mover_type,
        n.direction,n.speed,n.crush,n.front_tex,n.sector_id,n.floor_height,
        -- EV_DoFloor caps a raise at the sector's own ceiling
        LEAST(MIN(n.other_ceil),n.ceil_height) AS lowest_ceiling,
        MIN(CASE WHEN n.other_floor>n.floor_height THEN n.other_floor END) AS next_floor,
        MIN(sh.shortest) AS shortest_texture
      FROM neighbors n
      LEFT JOIN shortest sh ON sh.sector_id=n.sector_id
      GROUP BY n.map_id,n.line_id,n.special,n.height_target,n.change_tex,n.mover_type,
        n.direction,n.speed,n.crush,n.front_tex,n.sector_id,n.floor_height,n.ceil_height
    ),
    dest AS (
      SELECT t.*,
        CASE t.height_target
          WHEN 'lowest_ceiling' THEN t.lowest_ceiling
          WHEN 'lowest_ceiling_minus8' THEN t.lowest_ceiling-8
          WHEN 'plus24' THEN t.floor_height+24
          WHEN 'plus32' THEN t.floor_height+32
          WHEN 'plus512' THEN t.floor_height+512
          WHEN 'shortest_texture' THEN t.floor_height+t.shortest_texture
          ELSE t.next_floor END::smallint AS destination
      FROM targets t
    ),
    model AS (
      SELECT d.*,
        CASE WHEN d.height_target IN ('plus24','plus32') THEN d.front_tex ELSE
        (SELECT n.other_tex FROM neighbors n
          WHERE n.map_id=d.map_id AND n.line_id=d.line_id
            AND n.sector_id=d.sector_id AND n.other_floor=d.destination
          ORDER BY n.other_id LIMIT 1) END AS model_tex
      FROM dest d
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush)
    SELECT map_id,sector_id,line_id,mover_type,'floor',direction,
      floor_height,destination,speed,0,0,NULL,NULL,
      CASE WHEN change_tex THEN model_tex END,FALSE,crush
    FROM model
    WHERE destination IS NOT NULL AND destination>floor_height
    ON CONFLICT (map_id,sector_id) DO NOTHING;
    UPDATE sectors s
    SET floor_height=q.dest
    FROM (
      SELECT sec.map_id,sec.id,sec.floor_height,
             (LEAST(MIN(o.ceil_height),sec.ceil_height)
              -CASE WHEN d.height_target='lowest_ceiling_minus8' THEN 8 ELSE 0 END)::smallint AS dest
      FROM line_special_events e
      JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='raise'
       AND d.height_target IN ('lowest_ceiling','lowest_ceiling_minus8')
      JOIN sectors sec ON sec.map_id=e.map_id AND sec.tag=ld.tag AND ld.tag<>0
      JOIN sector_adjacency a ON a.map_id=sec.map_id AND a.sector_id=sec.id
      JOIN sectors o ON o.map_id=a.map_id AND o.id=a.other_id
      WHERE e.map_id=a_map_id AND o.id<>sec.id
      GROUP BY sec.map_id,sec.id,sec.floor_height,sec.ceil_height,d.height_target
    ) q
    WHERE s.map_id=q.map_id AND s.id=q.id AND q.dest<q.floor_height
      AND NOT EXISTS (SELECT 1 FROM sector_movers sm WHERE sm.map_id=s.map_id AND sm.sector_id=s.id);
  END IF;
  IF (v_flags & 256) <> 0 THEN
    -- Stairs (special 8). Doom walks OUTWARD IN A LINE from the tagged
    -- sector: at each step it takes the first two-sided line whose neighbour has
    -- the same floor flat, and raises that sector another 8 units.
    --
    -- Each step is an ordinary upward floor mover with its own target: vanilla
    -- spawns one floormove_t per step too, and they arrive staggered, which is
    -- what makes a staircase appear to build itself.
    WITH RECURSIVE
    events AS (
      SELECT e.map_id,e.line_id,ld.tag,
             COALESCE(d.step_height,8)::int AS step_h, COALESCE(d.speed,1) AS speed
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='stairs'
      WHERE e.map_id=a_map_id
    ),
    -- Sector adjacency, both ways, built once. Recursing straight over linedefs
    -- re-joined the whole map at every level.
    adj AS (
      SELECT linedef_id AS edge_id, sector_id AS a, other_id AS b
      FROM sector_adjacency WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    chain AS (
      SELECT e.map_id,e.line_id,s.id AS sector_id,s.floor_tex,0 AS step,
             (s.floor_height+e.step_h)::int AS destination,
             -- the sectors on the way as ',12,15,' (PostgreSQL's ARRAY[s.id], which MySQL has no type for)
             CAST(CONCAT(',', s.id, ',') AS CHAR(1024)) AS seen,
             e.step_h,e.speed
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      UNION ALL
      SELECT c.map_id,c.line_id,n.nid,n.ntex,c.step+1,c.destination+c.step_h,
             CONCAT(c.seen, n.nid, ','),c.step_h,c.speed
      FROM chain c
      CROSS JOIN LATERAL (
        SELECT nxt.id AS nid,nxt.floor_tex AS ntex
        FROM adj JOIN sectors nxt ON nxt.map_id=a_map_id AND nxt.id=adj.b
        WHERE adj.a=c.sector_id AND nxt.floor_tex=c.floor_tex
          AND LOCATE(CONCAT(',', nxt.id, ','), c.seen) = 0
        ORDER BY adj.edge_id LIMIT 1
      ) n
      WHERE c.step<24
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT ch.map_id,ch.sector_id,ch.line_id,'floor_raise','floor',1,
      s.floor_height,ch.destination::smallint,ch.speed,0,0,NULL,NULL,NULL,FALSE
    FROM chain ch
    JOIN sectors s ON s.map_id=ch.map_id AND s.id=ch.sector_id
    WHERE ch.destination>s.floor_height
    ON CONFLICT (map_id,sector_id) DO NOTHING;
  END IF;
  IF (v_flags & 512) <> 0 THEN
    -- W1 light change.
    WITH ev AS (
      SELECT ld.tag,d.target_light,d.light_source
      FROM line_special_events e
      JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='lights'
      WHERE e.map_id=a_map_id AND (d.light_source IS NULL OR d.light_source <> 'strobe')
    ),
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    -- EV_LightTurnOn: a fixed level, or the brightest / dimmest neighbour.
    neighbour_light AS (
      SELECT a.sector_id, MAX(o.light_level) AS brightest, MIN(o.light_level) AS dimmest
      FROM adjacency a JOIN sectors o ON o.map_id=a_map_id AND o.id=a.other_id
      GROUP BY a.sector_id
    ),
    newlight AS (
      SELECT s.id AS sector_id,
        MAX(CASE ev.light_source
              WHEN 'max_neighbor' THEN nb.brightest
              WHEN 'min_neighbor' THEN nb.dimmest
              ELSE ev.target_light END) AS lvl
      FROM ev JOIN sectors s ON s.map_id=a_map_id AND s.tag=ev.tag
      LEFT JOIN neighbour_light nb ON nb.sector_id=s.id
      GROUP BY s.id
    )
    UPDATE sectors s SET light_level=n.lvl
    FROM newlight n
    WHERE s.map_id=a_map_id AND s.id=n.sector_id AND n.lvl IS NOT NULL;
    -- EV_StartLightStrobing: the tagged sectors flash like a special-3 sector
    -- (SLOWDARK 35 dark, STROBEBRIGHT 5 bright) between their own level and
    -- their dimmest neighbour's.
    INSERT INTO sector_light_fx (map_id, sector_id, special, base_light, dark_light)
    SELECT s.map_id, s.id, 3::smallint, s.light_level,
           LEAST(s.light_level, COALESCE((SELECT MIN(o.light_level)
             FROM sector_adjacency a
             JOIN sectors o ON o.map_id=a.map_id AND o.id=a.other_id
             WHERE a.map_id=s.map_id AND a.sector_id=s.id
               AND a.other_id<>s.id), 0::smallint))::smallint
    FROM line_special_events e
    JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
    JOIN line_special_defs d ON d.special=ld.special AND d.light_source='strobe'
    JOIN sectors s ON s.map_id=e.map_id AND s.tag=ld.tag
    WHERE e.map_id=a_map_id
    ON CONFLICT (map_id, sector_id) DO NOTHING;
    UPDATE render_segs rs SET f_light=s.light_level
    FROM sectors s, line_special_events e
    JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
    JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='lights'
    WHERE e.map_id=a_map_id
      AND s.map_id=e.map_id AND s.tag=ld.tag
      AND rs.map_id=s.map_id AND rs.fsec=s.id;
    UPDATE render_segs rs SET b_light=s.light_level
    FROM sectors s, line_special_events e
    JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
    JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='lights'
    WHERE e.map_id=a_map_id
      AND s.map_id=e.map_id AND s.tag=ld.tag
      AND rs.map_id=s.map_id AND rs.bsec=s.id;
  END IF;
  IF (v_flags & 1024) <> 0 THEN
    -- TELEPORT (39 W1, 97 WR). Doom looks for a Thing of type 14 sitting in a
    -- sector whose tag matches the line, then drops the player on it facing the
    -- way that Thing faces, with momentum cleared.
    --
    -- Type 14 Things are invisible, so they never reach render_things and carry
    -- no sector_id. Rather than run a BSP descent per destination, the
    -- destination is matched to the tagged sector by its segs' bounding box and,
    -- where a tag covers more than one sector, by the closest box centre. Doom's
    -- teleport closets are small and hold exactly one destination, so this picks
    -- the same Thing the original would.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,e.player_thing_id,ld.tag
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='teleport'
      WHERE e.map_id=a_map_id
    ),
    tagged AS (
      SELECT e.map_id,e.line_id,e.player_thing_id,s.id AS sector_id,s.floor_height,
             MIN(rs.x1) AS min_x, MAX(rs.x1) AS max_x,
             MIN(rs.y1) AS min_y, MAX(rs.y1) AS max_y
      FROM events e
      JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      JOIN render_segs rs ON rs.map_id=s.map_id AND rs.fsec=s.id
      GROUP BY e.map_id,e.line_id,e.player_thing_id,s.id,s.floor_height
    ),
    candidates AS (
      SELECT t.*, th.id AS dest_id, th.x AS dest_x, th.y AS dest_y, th.angle AS dest_angle,
             ABS(th.x - (t.min_x+t.max_x)/2.0) + ABS(th.y - (t.min_y+t.max_y)/2.0) AS off_centre
      FROM tagged t
      JOIN things th ON th.map_id=t.map_id
      JOIN thing_role_defs tr ON tr.thing_type=th.type AND tr.is_teleport_dest
       AND th.x BETWEEN t.min_x AND t.max_x
       AND th.y BETWEEN t.min_y AND t.max_y
    ),
    chosen AS (
      SELECT * FROM (
        SELECT c.*, ROW_NUMBER() OVER
          (PARTITION BY c.map_id,c.player_thing_id ORDER BY c.off_centre, c.dest_id) AS rn
        FROM candidates c) q
      WHERE rn=1
    )
    UPDATE player_state ps
    SET position_x=ch.dest_x, position_y=ch.dest_y,
        -- previous_* follows the destination too: leaving it at the old position
        -- would make doom_cs_cross think the player had walked the whole way and
        -- fire every special linedef along the straight line between them.
        previous_x=ch.dest_x, previous_y=ch.dest_y,
        base_z=ch.floor_height+41.0, view_z=ch.floor_height+41.0,
        view_angle=ch.dest_angle,
        previous_view_z=ch.floor_height+41.0, previous_view_angle=ch.dest_angle,
        momentum_x=0, momentum_y=0, bob_strength=0,
        sector_id=ch.sector_id
    FROM chosen ch
    WHERE ps.map_id=ch.map_id AND ps.player_thing_id=ch.player_thing_id;
    UPDATE things t
    SET x=ps.position_x, y=ps.position_y, z=ps.view_z, angle=ps.view_angle
    FROM player_state ps
    WHERE t.map_id=a_map_id AND ps.map_id=t.map_id AND ps.player_thing_id=t.id
      AND EXISTS (SELECT 1 FROM line_special_events e
                  JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
                  JOIN line_special_defs d ON d.special=ld.special
                    AND d.mechanic='teleport'
                  WHERE e.map_id=t.map_id AND e.player_thing_id=t.id);
  END IF;
  IF (v_flags & 2048) <> 0 THEN
    -- Ceilings on their own: up to the highest neighbouring ceiling (40), or down
    -- to the floor (41, 43) / eight above it with a crush (44, 72).
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.tag,d.height_target,d.mover_type,d.direction,d.speed,d.crush
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='ceiling'
      WHERE e.map_id=a_map_id
    ),
    adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id AND sector_id<>other_id
    ),
    targets AS (
      SELECT e.map_id,e.line_id,e.height_target,e.mover_type,e.direction,e.speed,e.crush,
        s.id AS sector_id,s.floor_height,s.ceil_height,
        MAX(other.ceil_height) AS highest_ceiling
      FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag
      LEFT JOIN adjacency a ON a.sector_id=s.id
      LEFT JOIN sectors other ON other.map_id=s.map_id AND other.id=a.other_id
      GROUP BY e.map_id,e.line_id,e.height_target,e.mover_type,e.direction,e.speed,e.crush,
        s.id,s.floor_height,s.ceil_height
    ),
    targets_unique AS (
      SELECT * FROM (
        SELECT t.*,ROW_NUMBER() OVER (PARTITION BY t.map_id,t.sector_id ORDER BY t.line_id) AS rn
        FROM targets t) q WHERE rn=1
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush)
    SELECT map_id,sector_id,line_id,mover_type,'ceiling',direction,
      CASE height_target WHEN 'highest_ceiling' THEN ceil_height
                         WHEN 'floor8' THEN floor_height+8 ELSE floor_height END,
      CASE height_target WHEN 'highest_ceiling' THEN GREATEST(ceil_height,COALESCE(highest_ceiling,ceil_height))
                         ELSE ceil_height END,
      speed,0,0,NULL,NULL,NULL,FALSE,crush
    FROM targets_unique
    WHERE (direction=1 AND COALESCE(highest_ceiling,ceil_height)>ceil_height)
       OR (direction=-1 AND ceil_height>CASE height_target WHEN 'floor8' THEN floor_height+8 ELSE floor_height END)
    ON CONFLICT (map_id,sector_id) DO NOTHING;
  END IF;
  IF (v_flags & 4096) <> 0 THEN
    -- Crushers: the ceiling runs between its height and eight above the floor
    -- until a stop line; a stopped one resumes on the next activation.
    WITH
    events AS (
      SELECT e.map_id,e.line_id,ld.tag,d.speed
      FROM line_special_events e JOIN linedefs ld
        ON ld.map_id=e.map_id AND ld.id=e.line_id
      JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='crusher'
      WHERE e.map_id=a_map_id
    ),
    targets_unique AS (
      SELECT * FROM (
        SELECT e.map_id,e.line_id,e.speed,s.id AS sector_id,s.floor_height,s.ceil_height,
          ROW_NUMBER() OVER (PARTITION BY e.map_id,s.id ORDER BY e.line_id) AS rn
        FROM events e JOIN sectors s ON s.map_id=e.map_id AND s.tag=e.tag) q
      WHERE rn=1
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush)
    SELECT map_id,sector_id,line_id,'crusher','ceiling',-1,
      floor_height+8,ceil_height,speed,0,0,NULL,NULL,NULL,FALSE,TRUE
    FROM targets_unique WHERE ceil_height>floor_height+8
    ON CONFLICT (map_id,sector_id) DO UPDATE SET
      source_line_id=EXCLUDED.source_line_id,direction=-1,countdown=0,
      next_ceiling=NULL,moved_this_tick=FALSE
    WHERE sector_movers.mover_type='crusher' AND sector_movers.direction=2;
  END IF;
  IF (v_flags & 8192) <> 0 THEN
    -- EV_CeilingCrushStop / EV_StopPlat: freeze the tagged movers of that kind.
    UPDATE sector_movers m
    SET direction=2, moved_this_tick=FALSE
    FROM line_special_events e
    JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
    JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='stop'
    JOIN sectors s ON s.map_id=e.map_id AND s.tag=ld.tag
    WHERE e.map_id=a_map_id AND m.map_id=s.map_id AND m.sector_id=s.id
      AND m.mover_type=d.stops_mover AND m.direction<>2;
  END IF;
  DELETE FROM line_special_events WHERE map_id=a_map_id;
  SET v_active = 0::bigint;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT (SELECT count(*)::int FROM sector_movers
              WHERE map_id=a_map_id AND direction<>2) AS active_movers,
             (SELECT count(*)::int FROM line_buttons
              WHERE map_id=a_map_id AND countdown>0) AS active_buttons;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_active_movers, v_active_buttons;
      IF b2_done THEN LEAVE b2_each; END IF;
      IF v_active_movers > 0 THEN
        SET v_active = v_active + 1;
      END IF;
      IF v_active_buttons > 0 THEN
        SET v_active = v_active + 2;
      END IF;
    END LOOP;
    CLOSE b2_rows;
  END;
  RETURN v_active;
END;;

-- ==== 13_cs_doors.sql ====
DROP FUNCTION IF EXISTS doom_cs_doors;;
CREATE FUNCTION doom_cs_doors(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_any_restored BOOLEAN;
  DECLARE v_e BOOLEAN;
  -- Advance active ceiling/floor movers and repeatable switch buttons for one
  -- canonical 35 Hz tic.
  UPDATE line_buttons
  SET just_restored=(countdown=1),countdown=GREATEST(0,countdown-1)
  WHERE map_id=a_map_id AND countdown>=0;
  SET v_any_restored = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM line_buttons WHERE map_id=a_map_id AND just_restored) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_any_restored = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_any_restored THEN
    WITH original AS (
      SELECT DISTINCT map_id,sidedef_id
      FROM line_buttons WHERE map_id=a_map_id AND just_restored
    )
    UPDATE sidedefs sd
    SET upper_tex=COALESCE(u.original_tex,sd.upper_tex),
        mid_tex=COALESCE(m.original_tex,sd.mid_tex),
        lower_tex=COALESCE(l.original_tex,sd.lower_tex)
    FROM original o
    LEFT JOIN line_buttons u ON u.map_id=o.map_id AND u.sidedef_id=o.sidedef_id
      AND u.texture_part='upper'
    LEFT JOIN line_buttons m ON m.map_id=o.map_id AND m.sidedef_id=o.sidedef_id
      AND m.texture_part='middle'
    LEFT JOIN line_buttons l ON l.map_id=o.map_id AND l.sidedef_id=o.sidedef_id
      AND l.texture_part='lower'
    WHERE sd.map_id=o.map_id AND sd.id=o.sidedef_id;
    UPDATE render_segs rs
    SET upper_tex=sd.upper_tex,mid_tex=sd.mid_tex,lower_tex=sd.lower_tex
    FROM linedefs ld,sidedefs sd
    WHERE rs.map_id=a_map_id AND ld.map_id=rs.map_id AND ld.id=rs.linedef_id
      AND sd.map_id=ld.map_id
      AND sd.id=CASE WHEN rs.direction=0 THEN ld.right_sd_id ELSE ld.left_sd_id END
      AND EXISTS (SELECT 1 FROM line_buttons b
        WHERE b.map_id=rs.map_id AND b.line_id=rs.linedef_id AND b.just_restored);
  END IF;
  BEGIN
    DECLARE v_has_movers BOOLEAN DEFAULT FALSE;
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM sector_movers WHERE map_id=a_map_id AND direction<>2) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_e;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_has_movers = v_e;
    END LOOP;
    CLOSE b2_rows;
    IF NOT v_has_movers THEN
      RETURN 0;
    END IF;
  END;
  WITH
  params AS (
    SELECT t.map_id,t.x::float8 AS px,t.y::float8 AS py
    FROM things t
    WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int
  ),
  player_sector AS (
    SELECT doom_sector_at(p.map_id,p.px,p.py) AS sector_id FROM params p
  ),
  occupied_sectors AS (
    SELECT DISTINCT COALESCE(ai.sector_id, rt.sector_id) AS sector_id
    FROM monster_ai ai
    JOIN params p ON ai.map_id=p.map_id
    JOIN thing_health h ON h.map_id=ai.map_id AND h.thing_id=ai.thing_id
     AND h.alive
    JOIN render_things rt ON rt.map_id=ai.map_id AND rt.thing_id=ai.thing_id
    UNION
    -- The other players of a deathmatch; this player is player_sector above.
    SELECT ps.sector_id
    FROM player_state ps JOIN params p ON ps.map_id=p.map_id
    WHERE ps.player_thing_id<>a_player_thing_id::int AND ps.alive
      AND ps.sector_id IS NOT NULL
  ),
  current_state AS (
    SELECT m.*,s.floor_height,s.ceil_height,
      -- T_VerticalDoor asks P_ChangeSector whether anything is in the way and
      -- reverses a closing door when something is, which PIT_ChangeSector
      -- decides for every mobj in the sector. A monster
      -- standing under the door holds it open the same way a player does. Only the
      -- close-and-wait door refuses to reverse.
      -- Close-and-stay doors, ceilings coming down and crushers do not go back
      -- up for an obstacle.
      CASE WHEN m.plane='ceiling' AND m.direction=-1
            AND m.mover_type NOT IN ('door_close_open','door_close','crusher','ceiling_lower')
            AND (ps.sector_id=m.sector_id OR occ.sector_id IS NOT NULL)
           THEN 1 ELSE m.direction END AS move_direction
    FROM sector_movers m JOIN params p ON p.map_id=m.map_id
    JOIN sectors s ON s.map_id=m.map_id AND s.id=m.sector_id
    LEFT JOIN player_sector ps ON TRUE
    LEFT JOIN occupied_sectors occ ON occ.sector_id=m.sector_id
    WHERE m.direction<>2
  ),
  -- Vanilla moves a plane in 16.16 fixed point, so PLATSPEED/2 advances it half
  -- a map unit a tic. Heights here are whole units, so spend what the speed
  -- affords this tic and carry the rest: a 0.5 mover steps one unit every other
  -- tic, which averages the speed vanilla runs at.
  stepped AS (
    SELECT c.*,
      TRUNC(c.speed + c.move_carry)::smallint AS step_units,
      (c.speed + c.move_carry) - TRUNC(c.speed + c.move_carry) AS carry_out
    FROM current_state c
  ),
  motion AS (
    SELECT c.*,
      CASE WHEN c.plane='ceiling' THEN
        CASE c.move_direction WHEN 1 THEN LEAST(c.top_height,c.ceil_height+c.step_units)
          WHEN -1 THEN GREATEST(c.bottom_height,c.ceil_height-c.step_units)
          ELSE c.ceil_height END
        ELSE c.ceil_height END::smallint AS computed_ceiling,
      CASE WHEN c.plane='floor' THEN
        CASE c.move_direction WHEN 1 THEN LEAST(c.top_height,c.floor_height+c.step_units)
          WHEN -1 THEN GREATEST(c.bottom_height,c.floor_height-c.step_units)
          ELSE c.floor_height END
        ELSE c.floor_height END::smallint AS computed_floor
    FROM stepped c
  ),
  next_state AS (
    SELECT m.*,
      CASE
        WHEN m.move_direction=1
          AND ((m.plane='ceiling' AND m.computed_ceiling>=m.top_height)
            OR (m.plane='floor' AND m.computed_floor>=m.top_height))
          THEN CASE WHEN m.mover_type IN ('door_raise','door_manual_raise','platform_perpetual')
                    THEN 0
                    -- a crusher turns straight round at the top
                    WHEN m.mover_type='crusher' THEN -1
                    ELSE 2 END
        WHEN m.move_direction=0 AND m.countdown<=1
          THEN CASE WHEN m.mover_type IN ('platform','door_close_open') THEN 1
                    -- a perpetual platform heads back the way it did not come
                    WHEN m.mover_type='platform_perpetual'
                      THEN CASE WHEN m.floor_height>=m.top_height THEN -1 ELSE 1 END
                    ELSE -1 END
        WHEN m.move_direction=-1
          AND ((m.plane='ceiling' AND m.computed_ceiling<=m.bottom_height)
            OR (m.plane='floor' AND m.computed_floor<=m.bottom_height))
          THEN CASE WHEN m.mover_type IN ('platform','door_close_open','platform_perpetual')
                    THEN 0
                    WHEN m.mover_type='crusher' THEN 1
                    ELSE 2 END
        ELSE m.move_direction END::smallint AS computed_direction,
      CASE
        WHEN m.move_direction=1 AND m.mover_type IN ('door_raise','door_manual_raise','platform_perpetual')
          AND ((m.plane='ceiling' AND m.computed_ceiling>=m.top_height)
            OR (m.plane='floor' AND m.computed_floor>=m.top_height)) THEN m.wait_tics
        WHEN m.move_direction=-1 AND m.mover_type IN ('platform','platform_perpetual')
          AND m.computed_floor<=m.bottom_height THEN m.wait_tics
        -- The 30-second hold starts when the ceiling reaches the floor.
        WHEN m.move_direction=-1 AND m.mover_type='door_close_open'
          AND m.computed_ceiling<=m.bottom_height THEN m.wait_tics
        WHEN m.move_direction=0 THEN GREATEST(0,m.countdown-1)
        ELSE m.countdown END AS computed_countdown
    FROM motion m
  )
  UPDATE sector_movers mover
  SET direction=n.computed_direction,countdown=n.computed_countdown,
      -- A mover that has arrived or is waiting starts its next leg whole.
      move_carry=CASE WHEN n.computed_direction IN (0,2) THEN 0 ELSE n.carry_out END,
      next_ceiling=n.computed_ceiling,next_floor=n.computed_floor,
      moved_this_tick=(n.computed_ceiling<>n.ceil_height
        OR n.computed_floor<>n.floor_height)
  FROM next_state n
  WHERE mover.map_id=n.map_id AND mover.sector_id=n.sector_id;
  UPDATE sectors s SET ceil_height=m.next_ceiling
  FROM sector_movers m
  WHERE m.map_id=a_map_id AND s.map_id=m.map_id AND s.id=m.sector_id
    AND m.plane='ceiling' AND m.moved_this_tick;
  UPDATE sectors s
  SET floor_height=m.next_floor,
      floor_tex=CASE WHEN m.direction=2 AND m.target_floor_tex IS NOT NULL
                     THEN m.target_floor_tex ELSE s.floor_tex END
  FROM sector_movers m
  WHERE m.map_id=a_map_id AND s.map_id=m.map_id AND s.id=m.sector_id
    AND m.plane='floor' AND m.moved_this_tick;
  -- PIT_ChangeSector's crush: a crushing mover that has just closed the gap
  -- below a Thing's height hurts it for 10 every fourth tic.
  UPDATE thing_health h
  SET health=h.health-10, alive=h.health-10>0
  FROM sector_movers m
  JOIN sectors s ON s.map_id=m.map_id AND s.id=m.sector_id
  JOIN render_things rt ON rt.map_id=m.map_id AND rt.sector_id=m.sector_id
  JOIN things t ON t.map_id=rt.map_id AND t.id=rt.thing_id
  JOIN thing_combat_defs d ON d.thing_type=t.type AND NOT d.explodes
  JOIN player_state pc ON pc.map_id=m.map_id AND pc.player_thing_id=a_player_thing_id::int
  WHERE m.map_id=a_map_id AND m.crush AND m.moved_this_tick
    AND ((m.plane='ceiling' AND m.direction=-1) OR (m.plane='floor' AND m.direction=1))
    AND h.map_id=t.map_id AND h.thing_id=t.id AND h.alive
    AND s.ceil_height-s.floor_height < d.height
    AND pc.level_tics % 4 = 0;
  UPDATE player_state ps
  SET armor=ps.armor-d.saved,
      armor_class=CASE WHEN ps.armor-d.saved<=0 THEN 0 ELSE ps.armor_class END,
      health=GREATEST(0,ps.health-(10-d.saved)),
      damage_count=LEAST(100,ps.damage_count+(10-d.saved)),
      pain_face_tics=12,
      alive=(ps.health-(10-d.saved))>0,
      killer_id=CASE WHEN (ps.health-(10-d.saved))<=0 THEN -1 ELSE ps.killer_id END
  FROM (
    SELECT ps2.map_id, ps2.player_thing_id,
           LEAST(ps2.armor, CASE ps2.armor_class WHEN 2 THEN 5 WHEN 1 THEN 3 ELSE 0 END) AS saved
    FROM sector_movers m
    JOIN sectors s ON s.map_id=m.map_id AND s.id=m.sector_id
    JOIN player_state ps2 ON ps2.map_id=m.map_id AND ps2.sector_id=m.sector_id
    WHERE m.map_id=a_map_id AND m.crush AND m.moved_this_tick
      AND ((m.plane='ceiling' AND m.direction=-1) OR (m.plane='floor' AND m.direction=1))
      AND s.ceil_height-s.floor_height<56
      AND ps2.alive AND NOT ps2.god_mode AND ps2.invuln_tics<=0
      AND ps2.level_tics % 4 = 0
  ) d
  WHERE ps.map_id=d.map_id AND ps.player_thing_id=d.player_thing_id;
  UPDATE render_segs rs
  SET f_floor=s.floor_height,f_ceil=s.ceil_height,
      f_ceil_tex=s.ceil_tex,f_light=s.light_level
  FROM sectors s JOIN sector_movers m
    ON m.map_id=s.map_id AND m.sector_id=s.id
  WHERE rs.map_id=a_map_id AND rs.map_id=s.map_id
    AND m.moved_this_tick AND rs.fsec=s.id;
  UPDATE render_segs rs
  SET b_floor=s.floor_height,b_ceil=s.ceil_height,
      b_ceil_tex=s.ceil_tex,b_light=s.light_level
  FROM sectors s JOIN sector_movers m
    ON m.map_id=s.map_id AND m.sector_id=s.id
  WHERE rs.map_id=a_map_id AND rs.map_id=s.map_id
    AND m.moved_this_tick AND rs.bsec=s.id;
  RETURN 0;
END;;

-- ==== 14_cs_movement_mode.sql ====
DROP FUNCTION IF EXISTS doom_cs_movement_mode;;
CREATE FUNCTION doom_cs_movement_mode(a_map_id INT, a_player_thing_id INT) RETURNS TEXT
BEGIN
  DECLARE v_result_mode TEXT;
  DECLARE v_movement_mode TEXT;
  -- Select the movement implementation after this tic's special activation and
  -- sector-mover step.
  UPDATE game_tic_commands g
  SET movement_mode=CASE
    WHEN NOT ps.alive THEN 'idle'
    WHEN ABS(g.move_fwd)>0 OR ABS(g.move_strafe)>0
      OR ABS(ps.momentum_x)>0.0000001 OR ABS(ps.momentum_y)>0.0000001
      OR ABS(ps.momentum_z)>0.0000001
      OR (ps.base_z - 41.0 > s.floor_height + 0.5)
      OR COALESCE(g.jump_requested, FALSE)
      OR ps.bob_strength>0.0000001
      OR ABS(ps.view_z-ps.base_z)>0.0000001
      OR EXISTS (
        SELECT 1 FROM sector_movers sm
        WHERE sm.map_id=g.map_id AND sm.direction<>2
      )
      THEN 'full'
    WHEN ABS(g.turn_degrees)>0.0000001 THEN 'turn'
    ELSE 'idle'
  END
  FROM player_state ps
  JOIN sectors s ON s.map_id=ps.map_id AND s.id=ps.sector_id
  WHERE g.map_id=a_map_id::int AND g.player_thing_id=a_player_thing_id::int
    AND ps.map_id=g.map_id AND ps.player_thing_id=g.player_thing_id;
  -- Normalize the movement endpoints on an idle tic. Without this,
  -- previous_x/y would still describe the last moving tic and SQL would
  -- repeatedly rediscover the same crossing/pickup on every idle tic.
  UPDATE player_state ps
  SET previous_x=ps.position_x,previous_y=ps.position_y,
      momentum_x=0,momentum_y=0,bob_strength=0,
      view_z=CASE WHEN ps.alive THEN ps.base_z ELSE ps.view_z END,
      previous_view_z=CASE WHEN ps.alive THEN ps.base_z ELSE ps.view_z END,
      previous_view_angle=ps.view_angle
  FROM game_tic_commands g
  WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
    AND g.map_id=ps.map_id AND g.player_thing_id=ps.player_thing_id
    AND g.movement_mode='idle';
  SET v_result_mode = 'idle'::text;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT movement_mode
      FROM game_tic_commands
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_movement_mode;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_result_mode = v_movement_mode;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_result_mode;
END;;

-- ==== 15_cs_move.sql ====
DROP FUNCTION IF EXISTS doom_cs_move;;
CREATE FUNCTION doom_cs_move(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_player_radius DOUBLE;
  DECLARE v_player_height DOUBLE;
  DECLARE v_max_step DOUBLE;
  DECLARE v_view_height DOUBLE;
  DECLARE v_friction DOUBLE;
  DECLARE v_stop_speed DOUBLE;
  DECLARE v_max_move DOUBLE;
  DECLARE v_forwardmove DOUBLE;
  DECLARE v_forwardmove_run DOUBLE;
  DECLARE v_sidemove DOUBLE;
  DECLARE v_sidemove_run DOUBLE;
  DECLARE v_thrust_unit DOUBLE;
  DECLARE v_max_bob DOUBLE;
  DECLARE v_gravity DOUBLE;
  DECLARE v_bob_factor DOUBLE;
  DECLARE v_bob_period DOUBLE;
  DECLARE v_block_margin DOUBLE;
  DECLARE v_pos_epsilon DOUBLE;
  SET v_player_radius = doom_const('PLAYER_RADIUS');
  SET v_player_height = doom_const('PLAYER_HEIGHT');
  SET v_max_step = doom_const('MAXSTEP');
  SET v_view_height = doom_const('VIEWHEIGHT');
  SET v_friction = doom_const('FRICTION');
  SET v_stop_speed = doom_const('STOPSPEED');
  SET v_max_move = doom_const('MAXMOVE');
  SET v_forwardmove = doom_const('FORWARDMOVE');
  SET v_forwardmove_run = doom_const('FORWARDMOVE_RUN');
  SET v_sidemove = doom_const('SIDEMOVE');
  SET v_sidemove_run = doom_const('SIDEMOVE_RUN');
  SET v_thrust_unit = doom_const('THRUST_UNIT');
  SET v_max_bob = doom_const('MAXBOB');
  SET v_gravity = doom_const('GRAVITY');
  SET v_bob_factor = doom_const('BOB_FACTOR');
  SET v_bob_period = doom_const('BOB_PERIOD_TICS');
  SET v_block_margin = doom_const('BLOCK_MARGIN');
  SET v_pos_epsilon = doom_const('POS_EPSILON');
  -- One prepared 35 Hz player movement tick.
  --
  WITH
  params AS (
    SELECT
      g.map_id,
      g.player_thing_id,
      g.move_fwd::float8 AS move_fwd,
      g.move_strafe::float8 AS move_side,
      g.running,
      g.turn_degrees::float8 AS turn_degrees,
      COALESCE(g.jump_requested, FALSE) AS jump_requested,
      v_player_radius AS radius,
      ps2.noclip,
      v_player_height AS player_h,
      v_max_step AS max_step,
      v_view_height AS eye_h,
      v_friction AS friction,
      v_stop_speed AS stop_speed,
      v_max_move AS max_move,
      v_max_bob AS max_bob,
      v_gravity AS gravity
    FROM game_tic_commands g
    JOIN player_state ps2 ON ps2.map_id=g.map_id
     AND ps2.player_thing_id=g.player_thing_id
    WHERE g.map_id=a_map_id::int AND g.player_thing_id=a_player_thing_id::int
      AND g.movement_mode='full'
  ),
  current_pose AS (
    SELECT p.*,t.x::float8 AS px,t.y::float8 AS py,t.angle::float8 AS old_angle,
           ps.momentum_x::float8 AS old_mom_x,
           ps.momentum_y::float8 AS old_mom_y,
           ps.momentum_z::float8 AS old_mom_z,
           (ps.base_z-p.eye_h)::float8 AS feet_z,
           (ps.momentum_z=0 AND (ps.base_z-p.eye_h) <= s.floor_height + 0.5) AS on_ground,
           ps.level_tics,
           CASE WHEN p.running THEN v_forwardmove_run ELSE v_forwardmove END
             * v_thrust_unit AS forward_thrust,
           CASE WHEN p.running THEN v_sidemove_run ELSE v_sidemove END
             * v_thrust_unit AS side_thrust
    FROM params p
    JOIN things t ON t.map_id=p.map_id AND t.id=p.player_thing_id
    JOIN player_state ps ON ps.map_id=p.map_id
      AND ps.player_thing_id=p.player_thing_id
    JOIN sectors s ON s.map_id=ps.map_id AND s.id=ps.sector_id
  ),
  turned AS (
    SELECT c.*,
           (c.old_angle-c.turn_degrees)
             - 360.0*FLOOR((c.old_angle-c.turn_degrees)/360.0) AS new_angle
    FROM current_pose c
  ),
  thrusted AS (
    SELECT t.*,
           LEAST(t.max_move,GREATEST(-t.max_move,
             t.old_mom_x
             + CASE WHEN t.on_ground THEN
                 t.move_fwd*t.forward_thrust*COS(RADIANS(t.new_angle))
                 + t.move_side*t.side_thrust*SIN(RADIANS(t.new_angle))
               ELSE
                 0.35 * (t.move_fwd*t.forward_thrust*COS(RADIANS(t.new_angle))
                         + t.move_side*t.side_thrust*SIN(RADIANS(t.new_angle)))
               END)) AS raw_mom_x,
           LEAST(t.max_move,GREATEST(-t.max_move,
             t.old_mom_y
             + CASE WHEN t.on_ground THEN
                 t.move_fwd*t.forward_thrust*SIN(RADIANS(t.new_angle))
                 - t.move_side*t.side_thrust*COS(RADIANS(t.new_angle))
               ELSE
                 0.35 * (t.move_fwd*t.forward_thrust*SIN(RADIANS(t.new_angle))
                         - t.move_side*t.side_thrust*COS(RADIANS(t.new_angle)))
               END)) AS raw_mom_y
    FROM turned t
  ),
  lines_here AS (
    SELECT ld.linedef_id AS id,ld.flags,ld.left_sd_id,ld.right_sd_id,
           ld.x1::float8 AS x1,ld.y1::float8 AS y1,
           ld.x2::float8 AS x2,ld.y2::float8 AS y2,
           sf.floor_height AS f_floor,sf.ceil_height AS f_ceil,
           sb.floor_height AS b_floor,sb.ceil_height AS b_ceil
    FROM params p
    JOIN linedef_geom ld ON ld.map_id=p.map_id
    LEFT JOIN sectors sf ON sf.map_id=ld.map_id AND sf.id=ld.fsec
    LEFT JOIN sectors sb ON sb.map_id=ld.map_id AND sb.id=ld.bsec
  ),
  blocking AS (
    SELECT lg.x1,lg.y1,lg.x2,lg.y2
    FROM lines_here lg
    -- P_BlockLinesIterator only visits the lines in the blockmap cells the move
    -- touches. There is no blockmap here, so bound it the same way: every
    -- candidate position lies between (px,py) and (px+mom,py+mom), and a line
    -- can only block one by coming within the 16-unit collision radius of it,
    -- so nothing outside this box is reachable this tic.
    WHERE LEAST(lg.x1,lg.x2)
            <= (SELECT GREATEST(t.px,t.px+t.raw_mom_x)+t.radius+2.0
                FROM thrusted t)
      AND GREATEST(lg.x1,lg.x2)
            >= (SELECT LEAST(t.px,t.px+t.raw_mom_x)-t.radius-2.0
                FROM thrusted t)
      AND LEAST(lg.y1,lg.y2)
            <= (SELECT GREATEST(t.py,t.py+t.raw_mom_y)+t.radius+2.0
                FROM thrusted t)
      AND GREATEST(lg.y1,lg.y2)
            >= (SELECT LEAST(t.py,t.py+t.raw_mom_y)-t.radius-2.0
                FROM thrusted t)
      -- The reasons a line blocks.
      AND (lg.left_sd_id=-1 OR lg.right_sd_id=-1 OR (lg.flags & 1)<>0
        -- P_TryMove: the opening has to be tall enough to stand in.
        OR (lg.b_ceil IS NOT NULL AND lg.f_ceil IS NOT NULL
            AND lg.b_floor IS NOT NULL AND lg.f_floor IS NOT NULL
            AND LEAST(lg.f_ceil,lg.b_ceil)-GREATEST(lg.f_floor,lg.b_floor)<v_player_height)
        -- P_TryMove blocks a step UP over 24 units, measured from the thing's
        -- feet. It does NOT block a drop.
        OR (lg.b_floor IS NOT NULL AND lg.f_floor IS NOT NULL
            AND GREATEST(lg.b_floor,lg.f_floor)
                > (SELECT c.feet_z+c.max_step FROM current_pose c)))
  ),
  -- PIT_CheckThing. Doom blocks on a bounding box: A move is refused when the
  -- two boxes overlap on BOTH axes.
  -- Only solid things block -- monsters and barrels while they live;
  -- P_KillMobj clears MF_SOLID, so a corpse is walked over.
  -- Solid decorations -- lamps, pillars, firesticks -- have no health to be
  -- alive or dead, so they come from thing_blocking_defs instead and block
  -- unconditionally.
  blocking_things AS (
    SELECT tt.x::float8 AS tx, tt.y::float8 AS ty,
           (COALESCE(cd.radius, bd.radius) + v_player_radius)::float8 AS blockdist
    FROM things tt
    LEFT JOIN thing_combat_defs cd ON cd.thing_type = tt.type
    LEFT JOIN thing_health hh ON hh.map_id = tt.map_id AND hh.thing_id = tt.id
    LEFT JOIN thing_blocking_defs bd ON bd.thing_type = tt.type
    WHERE tt.map_id = a_map_id::int
      AND tt.id <> a_player_thing_id::int
      AND (
        -- An actor, for as long as it lives.
        (cd.thing_type IS NOT NULL AND hh.alive)
        -- Or a decoration Doom marks MF_SOLID. One that the skill or the
        -- multiplayer flag kept from spawning is not in the level, so it
        -- cannot block either.
        OR (bd.thing_type IS NOT NULL AND (tt.flags & 16) = 0
            AND (tt.flags & (SELECT g.skill_bit FROM game_tic_commands g
                             WHERE g.map_id = a_map_id::int
                               AND g.player_thing_id = a_player_thing_id::int)) <> 0))
      AND tt.x <= (SELECT GREATEST(w.px,w.px+w.raw_mom_x) FROM thrusted w)
                  + COALESCE(cd.radius, bd.radius) + v_player_radius + v_block_margin
      AND tt.x >= (SELECT LEAST(w.px,w.px+w.raw_mom_x) FROM thrusted w)
                  - COALESCE(cd.radius, bd.radius) - v_player_radius - v_block_margin
      AND tt.y <= (SELECT GREATEST(w.py,w.py+w.raw_mom_y) FROM thrusted w)
                  + COALESCE(cd.radius, bd.radius) + v_player_radius + v_block_margin
      AND tt.y >= (SELECT LEAST(w.py,w.py+w.raw_mom_y) FROM thrusted w)
                  - COALESCE(cd.radius, bd.radius) - v_player_radius - v_block_margin
    UNION ALL
    -- The other players of a deathmatch, MF_SOLID with a 16-unit radius.
    SELECT ot.x::float8, ot.y::float8, (v_player_radius*2)::float8
    FROM player_state op
    JOIN things ot ON ot.map_id=op.map_id AND ot.id=op.player_thing_id
    WHERE op.map_id = a_map_id::int
      AND op.player_thing_id <> a_player_thing_id::int AND op.alive
  ),

  candidates AS (
    SELECT 1 AS pri,t.px+t.raw_mom_x AS cx,t.py+t.raw_mom_y AS cy
    FROM thrusted t
    UNION ALL
    SELECT 2,t.px+t.raw_mom_x,t.py FROM thrusted t
    UNION ALL
    SELECT 3,t.px,t.py+t.raw_mom_y FROM thrusted t
    UNION ALL
    SELECT 4,t.px,t.py FROM thrusted t
  ),
  candidate_blocked AS (
    SELECT c.pri,c.cx,c.cy,
           (count(*) FILTER (WHERE d.dist<p.radius AND d.dist<d0.dist)>0
             OR count(*) FILTER (WHERE xing.intersects)>0
             OR count(*) FILTER (WHERE ABS(c.cx-bt.tx) < bt.blockdist
                                   AND ABS(c.cy-bt.ty) < bt.blockdist)>0
             ) AND NOT p.noclip AS blocked
    FROM candidates c
    CROSS JOIN params p
    LEFT JOIN blocking b ON TRUE
    LEFT JOIN blocking_things bt ON TRUE
    CROSS JOIN LATERAL (
      SELECT LEAST(1.0,GREATEST(0.0,
        ((c.cx-b.x1)*(b.x2-b.x1)+(c.cy-b.y1)*(b.y2-b.y1))
        / NULLIF(POWER(b.x2-b.x1,2)+POWER(b.y2-b.y1,2),0.0)
      )) AS t
    ) lt
    CROSS JOIN LATERAL (
      SELECT SQRT(POWER(c.cx-(b.x1+lt.t*(b.x2-b.x1)),2)
                + POWER(c.cy-(b.y1+lt.t*(b.y2-b.y1)),2)) AS dist
    ) d
    CROSS JOIN LATERAL (
      SELECT LEAST(1.0,GREATEST(0.0,
        ((p0.px-b.x1)*(b.x2-b.x1)+(p0.py-b.y1)*(b.y2-b.y1))
        / NULLIF(POWER(b.x2-b.x1,2)+POWER(b.y2-b.y1,2),0.0)
      )) AS t
      FROM thrusted p0
    ) lt0
    CROSS JOIN LATERAL (
      SELECT SQRT(POWER(p0.px-(b.x1+lt0.t*(b.x2-b.x1)),2)
                + POWER(p0.py-(b.y1+lt0.t*(b.y2-b.y1)),2)) AS dist
      FROM thrusted p0
    ) d0
    CROSS JOIN LATERAL (
      SELECT
        (p0.px-b.x1)*(b.y2-b.y1)-(p0.py-b.y1)*(b.x2-b.x1) AS d1,
        (c.cx-b.x1)*(b.y2-b.y1)-(c.cy-b.y1)*(b.x2-b.x1) AS d2,
        (b.x1-p0.px)*(c.cy-p0.py)-(b.y1-p0.py)*(c.cx-p0.px) AS d3,
        (b.x2-p0.px)*(c.cy-p0.py)-(b.y2-p0.py)*(c.cx-p0.px) AS d4
      FROM thrusted p0
    ) sides
    CROSS JOIN LATERAL (
      SELECT sides.d1*sides.d2<0 AND sides.d3*sides.d4<0 AS intersects
    ) xing
    GROUP BY c.pri,c.cx,c.cy
  ),
  best_pos AS (
    SELECT cx,cy FROM candidate_blocked WHERE NOT blocked ORDER BY pri LIMIT 1
  ),
  sector_floor AS (
    SELECT s.id AS sector_id,s.floor_height,s.ceil_height
    FROM params p CROSS JOIN best_pos bp
    JOIN sectors s ON s.map_id=p.map_id
     AND s.id=doom_sector_at(p.map_id,bp.cx,bp.cy)
  ),
  resolved AS (
    SELECT t.*,bp.cx,bp.cy,sf.sector_id,
           z.feet+t.eye_h AS base_z,z.next_mom_z,
           LEAST((POWER(t.raw_mom_x,2)+POWER(t.raw_mom_y,2))/v_bob_factor,
                 t.max_bob) AS bob_strength,
           CASE WHEN ABS(bp.cx-(t.px+t.raw_mom_x))<v_pos_epsilon
                THEN t.raw_mom_x ELSE 0.0 END AS slide_mom_x,
           CASE WHEN ABS(bp.cy-(t.py+t.raw_mom_y))<v_pos_epsilon
                THEN t.raw_mom_y ELSE 0.0 END AS slide_mom_y
    FROM thrusted t CROSS JOIN best_pos bp CROSS JOIN sector_floor sf
    -- P_ZMovement: apply the pending fall or jump, land if that reaches the floor,
    -- bonk if that hits the ceiling, otherwise accelerate downward.
    CROSS JOIN LATERAL (
      SELECT
        CASE
          WHEN t.on_ground AND t.jump_requested AND sf.floor_height + 8.5 + t.player_h <= sf.ceil_height
            THEN sf.floor_height::float8 + 8.5
          WHEN t.feet_z + t.old_mom_z <= sf.floor_height
            THEN sf.floor_height::float8
          WHEN t.feet_z + t.old_mom_z + t.player_h >= sf.ceil_height
            THEN sf.ceil_height::float8 - t.player_h
          ELSE t.feet_z + t.old_mom_z
        END AS feet,
        CASE
          WHEN t.on_ground AND t.jump_requested AND sf.floor_height + 8.5 + t.player_h <= sf.ceil_height
            THEN 8.5 - t.gravity
          WHEN t.feet_z + t.old_mom_z <= sf.floor_height
            THEN 0.0
          WHEN t.feet_z + t.old_mom_z + t.player_h >= sf.ceil_height
            THEN -1.0 * t.gravity
          WHEN t.old_mom_z = 0.0
            THEN -2.0 * t.gravity
          ELSE t.old_mom_z - t.gravity
        END AS next_mom_z
    ) z
  ),
  motion AS (
    SELECT r.*,
           (r.bob_strength/2.0)
             * SIN(2.0*PI()*r.level_tics/v_bob_period) AS bob_offset,
           CASE
             WHEN NOT r.on_ground THEN r.slide_mom_x
             WHEN r.move_fwd=0 AND r.move_side=0
              AND ABS(r.slide_mom_x)<r.stop_speed
              AND ABS(r.slide_mom_y)<r.stop_speed THEN 0.0
             ELSE r.slide_mom_x*r.friction
           END AS next_mom_x,
           CASE
             WHEN NOT r.on_ground THEN r.slide_mom_y
             WHEN r.move_fwd=0 AND r.move_side=0
              AND ABS(r.slide_mom_x)<r.stop_speed
              AND ABS(r.slide_mom_y)<r.stop_speed THEN 0.0
             ELSE r.slide_mom_y*r.friction
           END AS next_mom_y
    FROM resolved r
  )
  UPDATE player_state ps
  SET previous_x=m.px,previous_y=m.py,
      position_x=m.cx,position_y=m.cy,
      base_z=m.base_z,view_z=m.base_z+m.bob_offset,
      view_angle=m.new_angle,
      previous_view_z=ps.view_z,previous_view_angle=ps.view_angle,
      momentum_x=m.next_mom_x,momentum_y=m.next_mom_y,
      momentum_z=m.next_mom_z,
      bob_strength=m.bob_strength,
      sector_id=m.sector_id
  FROM motion m
  WHERE ps.map_id=m.map_id AND ps.player_thing_id=m.player_thing_id;
  UPDATE things t
  SET x=ps.position_x,y=ps.position_y,z=ps.view_z,angle=ps.view_angle
  FROM player_state ps
  WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int
    AND ps.map_id=t.map_id AND ps.player_thing_id=t.id
    AND EXISTS (
      SELECT 1 FROM game_tic_commands g
      WHERE g.map_id=t.map_id AND g.player_thing_id=t.id
        AND g.movement_mode='full'
    );
  RETURN 0;
END;;

-- ==== 16_cs_turn.sql ====
DROP FUNCTION IF EXISTS doom_cs_turn;;
CREATE FUNCTION doom_cs_turn(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Prepared stationary turn fast path
  UPDATE player_state ps
  SET previous_x=t.x,previous_y=t.y,
      position_x=t.x,position_y=t.y,
      view_angle=(t.angle-g.turn_degrees::float8)
        -360.0*FLOOR((t.angle-g.turn_degrees::float8)/360.0),
      previous_view_angle=ps.view_angle,previous_view_z=ps.view_z,
      momentum_x=0,momentum_y=0,bob_strength=0,view_z=ps.base_z
  FROM things t,game_tic_commands g
  WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
    AND t.map_id=ps.map_id AND t.id=ps.player_thing_id
    AND g.map_id=ps.map_id AND g.player_thing_id=ps.player_thing_id
    AND g.movement_mode='turn';
  UPDATE things t
  SET angle=ps.view_angle
  FROM player_state ps
  WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int
    AND ps.map_id=t.map_id AND ps.player_thing_id=t.id
    AND EXISTS (
      SELECT 1 FROM game_tic_commands g
      WHERE g.map_id=t.map_id AND g.player_thing_id=t.id
        AND g.movement_mode='turn'
    );
  RETURN 0;
END;;

-- ==== 17_cs_secret.sql ====
DROP FUNCTION IF EXISTS doom_cs_secret;;
CREATE FUNCTION doom_cs_secret(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Record first entry into a vanilla secret sector.
  WITH params AS (
    SELECT ps.map_id,ps.player_thing_id,ps.sector_id
    FROM player_state ps
    WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
  )
  INSERT INTO level_secret_discoveries (map_id,player_thing_id,sector_id)
  SELECT p.map_id,p.player_thing_id,p.sector_id
  FROM params p
  JOIN sectors s ON s.map_id=p.map_id AND s.id=p.sector_id
  JOIN sector_special_defs sd ON sd.special=s.special AND sd.is_secret
  WHERE NOT EXISTS (
      SELECT 1 FROM level_secret_discoveries d
      WHERE d.map_id=p.map_id AND d.player_thing_id=p.player_thing_id
        AND d.sector_id=p.sector_id
    )
  ON CONFLICT DO NOTHING;
  RETURN 0;
END;;

-- ==== 18_cs_cross.sql ====
DROP FUNCTION IF EXISTS doom_cs_cross;;
CREATE FUNCTION doom_cs_cross(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Queue every player-crossable special linedef traversed by this tic's
  -- accepted movement.
  WITH
  params AS (
    SELECT ps.map_id,ps.player_thing_id,
           ps.previous_x::float8 AS ox,ps.previous_y::float8 AS oy,
           ps.position_x::float8 AS nx,ps.position_y::float8 AS ny
    FROM player_state ps
    WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
  ),
  crossed AS (
    SELECT ld.id AS line_id, ld.special, ld.cross_once,
      ((v2.x-v1.x)::float8*(p.oy-v1.y)::float8
        - (v2.y-v1.y)::float8*(p.ox-v1.x)::float8) < 0 AS from_front,
      ABS((v2.x-v1.x)::float8*(p.oy-v1.y)::float8
        - (v2.y-v1.y)::float8*(p.ox-v1.x)::float8) AS old_side,
      ((v2.x-v1.x)::float8*(p.ny-v1.y)::float8
        - (v2.y-v1.y)::float8*(p.nx-v1.x)::float8) AS new_side,
      ((p.nx-p.ox)*(v1.y-p.oy)::float8 - (p.ny-p.oy)*(v1.x-p.ox)::float8)
        AS v1_side,
      ((p.nx-p.ox)*(v2.y-p.oy)::float8 - (p.ny-p.oy)*(v2.x-p.ox)::float8)
        AS v2_side
    FROM params p
    JOIN linedefs ld ON ld.map_id=p.map_id
    JOIN vertexes v1 ON v1.map_id=ld.map_id AND v1.id=ld.v1_id
    JOIN vertexes v2 ON v2.map_id=ld.map_id AND v2.id=ld.v2_id
    WHERE ld.cross_activated
  ),
  eligible AS (
    SELECT c.* FROM crossed c CROSS JOIN params p
    WHERE c.old_side > 1e-7
      AND c.new_side <> 0
      AND ((c.from_front AND c.new_side > 0)
        OR (NOT c.from_front AND c.new_side < 0))
      -- The step must actually reach the segment, not merely its extension.
      AND c.v1_side * c.v2_side < 0
      AND NOT (
        -- W1 rather than WR: fires once and then stays activated.
        c.cross_once
        AND EXISTS (
          SELECT 1 FROM line_activations a
          WHERE a.map_id=p.map_id AND a.line_id=c.line_id
        )
      )
  )
  INSERT INTO line_special_events
    (map_id,player_thing_id,line_id,trigger_type,from_front)
  SELECT p.map_id,p.player_thing_id,e.line_id,'cross',e.from_front
  FROM eligible e CROSS JOIN params p
  ON CONFLICT (map_id,player_thing_id,line_id,trigger_type) DO NOTHING;
  RETURN 0;
END;;

-- ==== 19_cs_pickups.sql ====
DROP FUNCTION IF EXISTS doom_cs_pickups;;
CREATE FUNCTION doom_cs_pickups(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_pos_epsilon DOUBLE;
  DECLARE v_pickup_reach DOUBLE;
  DECLARE v_dropped_base DOUBLE;
  DECLARE v_bonus_add DOUBLE;
  DECLARE v_msg_tics DOUBLE;
  SET v_pos_epsilon = doom_const('POS_EPSILON');
  SET v_pickup_reach = doom_const('PICKUP_REACH');
  SET v_dropped_base = doom_const('DROPPED_THING_ID_BASE');
  SET v_bonus_add = doom_const('BONUSADD');
  SET v_msg_tics = doom_const('MESSAGE_TICS');
  -- Apply health/armor/ammo/weapon/key pickup.
  -- Eligibility is staged before effects are applied so a pickup that fills a
  -- stat is still removed, while an item that cannot help remains in the world.
  -- Stage touched items
  WITH params AS (
    SELECT ps.map_id,ps.position_x::float8 AS px,ps.position_y::float8 AS py,
           g.skill_bit AS skill,g.skill AS skill_level,ps.player_thing_id
    FROM player_state ps
    JOIN game_tic_commands g ON g.map_id=ps.map_id
      AND g.player_thing_id=ps.player_thing_id
    WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
      AND (ABS(ps.position_x-ps.previous_x)>v_pos_epsilon
        OR ABS(ps.position_y-ps.previous_y)>v_pos_epsilon)
  ),
  eligible AS (
    SELECT t.id AS thing_id,t.type AS thing_type,d.kind,
           SQRT(POWER(t.x-p.px,2)+POWER(t.y-p.py,2)) AS dist
    FROM params p
    JOIN things t ON t.map_id=p.map_id
    JOIN pickup_defs d ON d.thing_type=t.type
    JOIN player_state ps ON ps.map_id=p.map_id
      AND ps.player_thing_id=p.player_thing_id
    WHERE (t.flags & p.skill)<>0 AND (t.flags & 16)=0
      AND SQRT(POWER(t.x-p.px,2)+POWER(t.y-p.py,2))<=v_pickup_reach
      AND NOT EXISTS (
        SELECT 1 FROM picked_up_items pu
        WHERE pu.map_id=t.map_id AND pu.thing_id=t.id
      )
      AND CASE
        WHEN d.kind='health' THEN
          NOT d.only_when_below_cap OR ps.health<d.cap
        -- Green/blue armor stays if its fixed point value is not an upgrade;
        -- armor bonuses are always consumed, including at 200 points.
        WHEN d.kind='armor' THEN
          NOT d.is_set_min OR ps.armor<d.amount
        -- Any ammo: what the player holds of that type against its ceiling.
        WHEN d.kind IN ('bullets','shells','rockets','cells') THEN
          (CASE d.kind WHEN 'bullets' THEN ps.ammo_bullets
                       WHEN 'shells'  THEN ps.ammo_shells
                       WHEN 'rockets' THEN ps.ammo_rockets
                       ELSE ps.ammo_cells END)
          < (SELECT CASE WHEN ps.backpack THEN ad.backpack_cap ELSE ad.cap END
               FROM ammo_defs ad WHERE ad.ammo_type::text = d.kind::text)
        WHEN d.kind='weapon' THEN NOT EXISTS (
          SELECT 1 FROM player_weapon_owned o
          WHERE o.map_id=p.map_id AND o.player_thing_id=p.player_thing_id
            AND o.weapon_id=d.weapon_id
        )
        -- Always taken: Doom's P_TouchSpecialThing has no condition on any of these.
        WHEN d.kind IN ('backpack','key_blue','key_yellow','key_red',
                       'radsuit','invis','lightamp','powermap','invuln') THEN TRUE
        ELSE FALSE
      END
  ),
  ranked AS (
    SELECT e.*,ROW_NUMBER() OVER
      (PARTITION BY e.kind ORDER BY e.dist,e.thing_id) AS rn
    FROM eligible e
  ),
  selected_items AS (
    SELECT DISTINCT thing_id,thing_type FROM ranked WHERE rn=1
  )
  INSERT INTO pickup_touches
    (map_id,player_thing_id,thing_id,kind,amount,cap,is_set_min,
     weapon_id,armor_class)
  SELECT p.map_id,p.player_thing_id,s.thing_id,d.kind,
         -- P_GiveAmmo: "give double ammo in trainer mode, you'll need it in
         -- nightmare" -- sk_baby and sk_nightmare only, not sk_easy.
         CASE WHEN d.kind IN ('bullets','shells','rockets','cells') THEN
           COALESCE(CASE WHEN s.thing_id>=v_dropped_base THEN d.dropped_amount END,
                    d.amount)*CASE WHEN p.skill_level IN (0,4) THEN 2 ELSE 1 END
           WHEN d.kind='backpack'
             THEN CASE WHEN p.skill_level IN (0,4) THEN 2 ELSE 1 END
           ELSE d.amount END,
         d.cap,d.is_set_min,d.weapon_id,d.armor_class
  FROM selected_items s
  JOIN pickup_defs d ON d.thing_type=s.thing_type
  CROSS JOIN params p
  ON CONFLICT (map_id,player_thing_id,thing_id,kind) DO NOTHING;
  -- health, armor class, backpack, and keys
  WITH staged AS (
    SELECT
      MAX(CASE WHEN kind='health' AND is_set_min THEN amount END) AS health_min,
      SUM(CASE WHEN kind='health' AND NOT is_set_min THEN amount END) AS health_add,
      MIN(CASE WHEN kind='health' AND NOT is_set_min THEN cap END) AS health_cap,
      MAX(CASE WHEN kind='armor' AND is_set_min THEN amount END) AS armor_set,
      SUM(CASE WHEN kind='armor' AND NOT is_set_min THEN amount END) AS armor_add,
      MIN(CASE WHEN kind='armor' AND NOT is_set_min THEN cap END) AS armor_cap,
      MAX(CASE WHEN kind='armor' AND is_set_min THEN armor_class END) AS set_class,
      BOOL_OR(kind='armor' AND NOT is_set_min) AS got_armor_bonus,
      BOOL_OR(kind='backpack') AS got_backpack,
      BOOL_OR(kind='key_blue') AS got_blue,
      BOOL_OR(kind='key_yellow') AS got_yellow,
      BOOL_OR(kind='key_red') AS got_red,
      -- Powerups restart at 60 s instead of stacking.
      MAX(CASE WHEN kind='radsuit' THEN amount END) AS got_radsuit,
      MAX(CASE WHEN kind='invis' THEN amount END) AS got_invis,
      MAX(CASE WHEN kind='lightamp' THEN amount END) AS got_lightamp,
      MAX(CASE WHEN kind='powermap' THEN amount END) AS got_powermap,
      MAX(CASE WHEN kind='invuln' THEN amount END) AS got_invuln,
      count(*) AS taken,
      -- MIN so the line is stable when a tic takes two things at once.
      MIN(pm.message) AS message
    FROM pickup_touches pt
    LEFT JOIN pickup_messages pm ON pm.thing_type = (
      SELECT t.type FROM things t
      WHERE t.map_id=pt.map_id AND t.id=pt.thing_id)
    WHERE pt.map_id=a_map_id AND pt.player_thing_id=a_player_thing_id
  )
  UPDATE player_state ps
  SET
      -- P_TouchSpecialThing ends with player->bonuscount += BONUSADD (6) for
      -- every item taken, which is the gold blink in the renderer. The
      -- summary row exists only when something was actually picked up, so this
      -- fires exactly once per pickup tic.
      bonus_count=LEAST(100,ps.bonus_count+v_bonus_add*s.taken),
      health=CASE
        WHEN s.health_min IS NOT NULL THEN GREATEST(ps.health,s.health_min)
        WHEN s.health_add IS NOT NULL THEN LEAST(s.health_cap,ps.health+s.health_add)
        ELSE ps.health END,
      armor=CASE
        WHEN s.armor_set IS NOT NULL THEN s.armor_set
        WHEN s.armor_add IS NOT NULL THEN LEAST(s.armor_cap,ps.armor+s.armor_add)
        ELSE ps.armor END,
      armor_class=CASE
        WHEN s.set_class IS NOT NULL THEN s.set_class
        WHEN s.got_armor_bonus AND ps.armor_class=0 THEN 1
        ELSE ps.armor_class END,
      backpack=ps.backpack OR COALESCE(s.got_backpack,FALSE),
      key_blue=ps.key_blue OR COALESCE(s.got_blue,FALSE),
      key_yellow=ps.key_yellow OR COALESCE(s.got_yellow,FALSE),
      key_red=ps.key_red OR COALESCE(s.got_red,FALSE),
      radsuit_tics=GREATEST(ps.radsuit_tics,COALESCE(s.got_radsuit,0)),
      invis_tics=GREATEST(ps.invis_tics,COALESCE(s.got_invis,0)),
      light_amp_tics=GREATEST(ps.light_amp_tics,COALESCE(s.got_lightamp,0)),
      power_map=ps.power_map OR COALESCE(s.got_powermap,0)>0,
      invuln_tics=GREATEST(ps.invuln_tics,COALESCE(s.got_invuln,0)),
      message=COALESCE(s.message, ps.message),
      message_tics=CASE WHEN s.message IS NOT NULL THEN v_msg_tics
                        ELSE ps.message_tics END
  FROM staged s
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id;
  WITH gained AS (
    SELECT ad.ammo_type,
           SUM(CASE WHEN pt.kind::text = ad.ammo_type::text THEN pt.amount
                    WHEN pt.kind = 'backpack' THEN ad.backpack_gives * pt.amount
               END)::int AS amount,
           CASE WHEN ps.backpack THEN ad.backpack_cap ELSE ad.cap END AS cap
    FROM ammo_defs ad
    JOIN player_state ps ON ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id
    LEFT JOIN pickup_touches pt
      ON pt.map_id=ps.map_id AND pt.player_thing_id=ps.player_thing_id
    GROUP BY ad.ammo_type, ad.cap, ad.backpack_cap, ps.backpack
  )
  UPDATE player_state ps
  SET ammo_bullets=COALESCE((SELECT LEAST(g.cap,ps.ammo_bullets+g.amount)
                             FROM gained g WHERE g.ammo_type='bullets' AND g.amount IS NOT NULL),ps.ammo_bullets),
      ammo_shells =COALESCE((SELECT LEAST(g.cap,ps.ammo_shells+g.amount)
                             FROM gained g WHERE g.ammo_type='shells' AND g.amount IS NOT NULL),ps.ammo_shells),
      ammo_rockets=COALESCE((SELECT LEAST(g.cap,ps.ammo_rockets+g.amount)
                             FROM gained g WHERE g.ammo_type='rockets' AND g.amount IS NOT NULL),ps.ammo_rockets),
      ammo_cells  =COALESCE((SELECT LEAST(g.cap,ps.ammo_cells+g.amount)
                             FROM gained g WHERE g.ammo_type='cells' AND g.amount IS NOT NULL),ps.ammo_cells)
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id;
  -- consume exactly the staged Things
  INSERT INTO picked_up_items (map_id,thing_id)
  SELECT DISTINCT map_id,thing_id FROM pickup_touches
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id
  ON CONFLICT (map_id,thing_id) DO NOTHING;
  -- stage newly acquired weapon
  INSERT INTO pickup_grants (map_id,player_thing_id,weapon_id)
  SELECT DISTINCT pt.map_id,pt.player_thing_id,pt.weapon_id
  FROM pickup_touches pt
  WHERE pt.map_id=a_map_id AND pt.player_thing_id=a_player_thing_id AND pt.kind='weapon'
    AND NOT EXISTS (
      SELECT 1 FROM player_weapon_owned o
      WHERE o.map_id=pt.map_id AND o.player_thing_id=pt.player_thing_id
        AND o.weapon_id=pt.weapon_id
    )
  ON CONFLICT (map_id,player_thing_id,weapon_id) DO NOTHING;
  -- apply weapon ownership
  INSERT INTO player_weapon_owned (map_id,player_thing_id,weapon_id)
  SELECT map_id,player_thing_id,weapon_id FROM pickup_grants
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id
  ON CONFLICT (map_id,player_thing_id,weapon_id) DO NOTHING;
  RETURN 0;
END;;

-- ==== 20_cs_weapon_state.sql ====
DROP FUNCTION IF EXISTS doom_cs_weapon_state;;
CREATE FUNCTION doom_cs_weapon_state(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  UPDATE player_weapons w
  SET state=d.n_state,seq_index=d.n_seq,tics=d.n_tics,
      current_weapon=d.n_current_weapon,pending_weapon=d.n_pending_weapon,
      flash_seq_index=d.n_flash_seq,flash_tics=d.n_flash_tics,
      sx=d.n_sx,sy=d.n_sy,
      shot_serial=w.shot_serial+CASE WHEN d.fires_now THEN 1 ELSE 0 END,
      fired_this_tick=d.fires_now
  FROM doom_weapon_decision d
  WHERE w.map_id=a_map_id AND w.player_thing_id=a_player_thing_id
    AND d.map_id=w.map_id AND d.player_thing_id=w.player_thing_id;
  RETURN 0;
END;;

-- ==== 21_cs_weapon.sql ====
DROP FUNCTION IF EXISTS doom_cs_weapon;;
CREATE FUNCTION doom_cs_weapon(a_map_id INT, a_player_thing_id INT) RETURNS BIGINT
BEGIN
  DECLARE v_result_shot BIGINT;
  DECLARE v_state TEXT;
  DECLARE v_seq_index BIGINT;
  DECLARE v_flash_seq_index BIGINT;
  DECLARE v_flash_tics BIGINT;
  DECLARE v_sx DOUBLE;
  DECLARE v_sy DOUBLE;
  DECLARE v_shot_serial BIGINT;
  DECLARE v_fired_this_tick BOOLEAN;
  DECLARE v_current_weapon BIGINT;
  DECLARE v_pellet_count BIGINT;
  DECLARE v_max_range DOUBLE;
  DECLARE v_dmg_dice_count BIGINT;
  DECLARE v_dmg_dice_mult BIGINT;
  DECLARE v_effects_alive INT;
  DO doom_cs_weapon_state(a_map_id,a_player_thing_id);
  -- Deduct ammo for just fired shots
  UPDATE player_state ps
  SET ammo_bullets = ps.ammo_bullets
        - CASE WHEN pw.fired_this_tick AND wd.ammo_type = 'bullets' THEN wd.ammo_per_shot ELSE 0 END,
      ammo_shells = ps.ammo_shells
        - CASE WHEN pw.fired_this_tick AND wd.ammo_type = 'shells' THEN wd.ammo_per_shot ELSE 0 END,
      ammo_rockets = ps.ammo_rockets
        - CASE WHEN pw.fired_this_tick AND wd.ammo_type = 'rockets' THEN wd.ammo_per_shot ELSE 0 END,
      ammo_cells = ps.ammo_cells
        - CASE WHEN pw.fired_this_tick AND wd.ammo_type = 'cells' THEN wd.ammo_per_shot ELSE 0 END
  FROM player_weapons pw
  JOIN weapon_defs wd ON wd.weapon_id = pw.current_weapon
  WHERE ps.map_id = pw.map_id AND ps.player_thing_id = pw.player_thing_id
    AND pw.map_id = a_map_id AND pw.player_thing_id = a_player_thing_id;
  UPDATE world_effects
  SET age=age+1
  WHERE map_id=a_map_id;
  DELETE FROM world_effects
  WHERE map_id=a_map_id
    AND age>=CASE WHEN effect_type='blood' THEN 24
                  WHEN effect_type='bfg_spray' THEN 32
                  WHEN effect_type='ifog' THEN 30
                  WHEN effect_type='tfog' THEN 72 ELSE 16 END;
  SET v_result_shot = 0::bigint;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT pw.state, pw.seq_index, pw.flash_seq_index, pw.flash_tics,
             pw.sx, pw.sy, pw.shot_serial, pw.fired_this_tick, pw.current_weapon,
             wd.pellet_count, wd.max_range, wd.dmg_dice_count, wd.dmg_dice_mult,
             (SELECT count(*) FROM world_effects WHERE map_id = a_map_id)::int AS effects_alive
      FROM player_weapons pw
      JOIN weapon_defs wd ON wd.weapon_id = pw.current_weapon
      WHERE pw.map_id = a_map_id AND pw.player_thing_id = a_player_thing_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_state, v_seq_index, v_flash_seq_index, v_flash_tics, v_sx, v_sy, v_shot_serial, v_fired_this_tick, v_current_weapon, v_pellet_count, v_max_range, v_dmg_dice_count, v_dmg_dice_mult, v_effects_alive;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_result_shot = v_shot_serial;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_result_shot;
END;;

-- ==== 22_cs_hitscan_fire.sql ====
DROP FUNCTION IF EXISTS doom_cs_hitscan_fire;;
CREATE FUNCTION doom_cs_hitscan_fire(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_impact_count BIGINT;
  DECLARE v_player_radius DOUBLE;
  DECLARE v_player_height DOUBLE;
  DECLARE v_view_height DOUBLE;
  DECLARE v_attack_z_offset DOUBLE;
  DECLARE v_aim_spread DOUBLE;
  DECLARE v_autoaim_slope DOUBLE;
  DECLARE v_slope_unbounded DOUBLE;
  DECLARE v_spread_units DOUBLE;
  DECLARE v_pellets_hit INT;
  DECLARE v_monsters_killed INT;
  DECLARE v_impacts INT;
  SET v_impact_count = 0;
  SET v_player_radius = doom_const('PLAYER_RADIUS');
  SET v_player_height = doom_const('PLAYER_HEIGHT');
  SET v_view_height = doom_const('VIEWHEIGHT');
  SET v_attack_z_offset = doom_const('ATTACK_Z_OFFSET');
  SET v_aim_spread = doom_const('AIM_SPREAD_DEGREES');
  SET v_autoaim_slope = doom_const('AUTOAIM_SLOPE');
  SET v_slope_unbounded = doom_const('SLOPE_UNBOUNDED');
  SET v_spread_units = doom_const('GUNSHOT_SPREAD_UNITS');
  -- Fire one hitscan weapon blast (fist/pistol/shotgun/chaingun/chainsaw)
  -- SQLDoom keeps the hits with INSERT .. RETURNING inside a WITH (a data-modifying CTE, which MySQL has no
  -- form of) and counts what came back; here the INSERT runs first and the count reads the shot's rows.
  WITH
      input AS (
        SELECT pw.map_id,pw.player_thing_id AS player_id,
               RADIANS(t.angle::double precision) AS view_angle,
               (ps.base_z - v_view_height + v_player_height/2.0
                + v_attack_z_offset)::double precision AS shoot_z,
               pw.shot_serial,wd.pellet_count,wd.max_range,
               wd.dmg_dice_count,wd.dmg_dice_mult
        FROM player_weapons pw
        JOIN weapon_defs wd ON wd.weapon_id=pw.current_weapon
        JOIN things t ON t.map_id=pw.map_id AND t.id=pw.player_thing_id
        JOIN player_state ps ON ps.map_id=pw.map_id
          AND ps.player_thing_id=pw.player_thing_id
        WHERE pw.map_id=a_map_id::int AND pw.player_thing_id=a_player_thing_id::int
          AND pw.fired_this_tick AND wd.pellet_count>0
      ),
      shooter AS (
        SELECT i.*, t.x::double precision AS px, t.y::double precision AS py
        FROM input i
        JOIN things t ON t.map_id = i.map_id AND t.id = i.player_id
      ),
      targets AS (
        SELECT t.id AS thing_id, t.x::double precision AS x,
               t.y::double precision AS y, d.radius::double precision AS radius,
               s.floor_height::double precision AS base_z,
               rt.thing_height::double precision AS height, rt.sector_id,
               d.no_blood
        FROM input i
        JOIN thing_health h ON h.map_id = i.map_id AND h.alive
        JOIN things t ON t.map_id = h.map_id AND t.id = h.thing_id
        JOIN thing_combat_defs d ON d.thing_type = t.type
        JOIN render_things rt ON rt.map_id = t.map_id AND rt.thing_id = t.id
        JOIN sectors s ON s.map_id = rt.map_id AND s.id = rt.sector_id
        UNION ALL
        -- The other players of a deathmatch: a player's own radius and height,
        -- standing on their sector's floor, and they bleed.
        SELECT ps.player_thing_id, t.x::double precision, t.y::double precision,
               v_player_radius::double precision,
               (ps.base_z - v_view_height)::double precision,
               v_player_height::double precision, ps.sector_id, FALSE
        FROM input i
        JOIN player_state ps ON ps.map_id = i.map_id
         AND ps.player_thing_id <> i.player_id AND ps.alive
        JOIN things t ON t.map_id = ps.map_id AND t.id = ps.player_thing_id
      ),
      map_lines AS (
        SELECT ld.linedef_id AS line_id,
               ld.x1::double precision AS x1, ld.y1::double precision AS y1,
               ld.x2::double precision AS x2, ld.y2::double precision AS y2,
               ld.fsec AS right_sector, ld.bsec AS left_sector,
               GREATEST(rsec.floor_height, lsec.floor_height)::double precision
                 AS open_bottom,
               LEAST(rsec.ceil_height, lsec.ceil_height)::double precision AS open_top,
               rsec.floor_height<>lsec.floor_height AS floors_differ,
               rsec.ceil_height<>lsec.ceil_height AS ceils_differ
        FROM input i
        JOIN linedef_geom ld ON ld.map_id = i.map_id
        LEFT JOIN sectors rsec ON rsec.map_id = ld.map_id AND rsec.id = ld.fsec
        LEFT JOIN sectors lsec ON lsec.map_id = ld.map_id AND lsec.id = ld.bsec
      ),
      -- P_BulletSlope tries straight ahead, then one spread either side.
      aim_rays(priority, angle) AS (
        SELECT 0, 0.0::double precision
        UNION ALL SELECT 1, RADIANS(v_aim_spread)
        UNION ALL SELECT 2, RADIANS(-v_aim_spread)
      ),
      aim_target_intersections AS (
        SELECT a.priority, a.angle, t.*,
               g.along - SQRT(GREATEST(0.0, t.radius*t.radius - g.perp2)) AS distance
        FROM shooter p
        CROSS JOIN aim_rays a
        CROSS JOIN targets t
        CROSS JOIN LATERAL (
          SELECT
            (t.x-p.px)*COS(p.view_angle+a.angle)
              + (t.y-p.py)*SIN(p.view_angle+a.angle) AS along,
            POWER(-(t.x-p.px)*SIN(p.view_angle+a.angle)
              + (t.y-p.py)*COS(p.view_angle+a.angle), 2) AS perp2
        ) g
        WHERE g.along > 0 AND g.along <= p.max_range + t.radius
          AND g.perp2 <= t.radius*t.radius
      ),
      -- P_AimLineAttack carries a vertical slope window (starting at Doom's +-100/160 autoaim cone) and
      -- narrows it at every two-sided line the trace crosses -- up to the step, down
      -- to the header -- then aims at the middle of whatever band of the target is
      -- still inside the window.
      aim_window AS (
        SELECT h.*, p.shoot_z,
               COALESCE(w.solid,FALSE) AS solid,
               GREATEST(-v_autoaim_slope, COALESCE(w.max_bottom, -v_slope_unbounded)) AS bslope,
               LEAST(v_autoaim_slope, COALESCE(w.min_top, v_slope_unbounded)) AS tslope
        FROM aim_target_intersections h
        CROSS JOIN shooter p
        CROSS JOIN LATERAL (
          SELECT
            MAX(CASE WHEN l.right_sector IS NULL OR l.left_sector IS NULL
                       OR l.open_bottom>=l.open_top THEN 1 ELSE 0 END)=1 AS solid,
            MAX(CASE WHEN l.floors_differ
                     THEN (l.open_bottom-p.shoot_z)/q.ray_t END) AS max_bottom,
            MIN(CASE WHEN l.ceils_differ
                     THEN (l.open_top-p.shoot_z)/q.ray_t END) AS min_top
          FROM map_lines l
          CROSS JOIN LATERAL (
            SELECT
              ((l.x1-p.px)*(l.y2-l.y1) - (l.y1-p.py)*(l.x2-l.x1))
                / NULLIF(COS(p.view_angle+h.angle)*(l.y2-l.y1)
                  - SIN(p.view_angle+h.angle)*(l.x2-l.x1), 0.0) AS ray_t,
              ((l.x1-p.px)*SIN(p.view_angle+h.angle)
                  - (l.y1-p.py)*COS(p.view_angle+h.angle))
                / NULLIF(COS(p.view_angle+h.angle)*(l.y2-l.y1)
                  - SIN(p.view_angle+h.angle)*(l.x2-l.x1), 0.0) AS line_u
          ) q
          WHERE q.ray_t > 0 AND q.ray_t < h.distance
            AND q.line_u BETWEEN 0 AND 1
        ) w
        WHERE h.distance > 0 AND h.distance <= p.max_range
      ),
      visible_aim_targets AS (
        SELECT w.*,
               (LEAST(b.thing_top,w.tslope)
                + GREATEST(b.thing_bot,w.bslope)) / 2.0 AS aimslope
        FROM aim_window w
        CROSS JOIN LATERAL (
          SELECT (w.base_z+w.height-w.shoot_z)/w.distance AS thing_top,
                 (w.base_z-w.shoot_z)/w.distance AS thing_bot
        ) b
        WHERE NOT w.solid AND w.tslope > w.bslope
          AND b.thing_top >= w.bslope AND b.thing_bot <= w.tslope
      ),
      -- Free aim (the modern client's mouse look: player_state.view_pitch set): the shot goes where the player
      -- looks, along the pitch's slope. Without it, Doom's autoaim: the nearest target in the slope window.
      aim AS (
        SELECT COALESCE((
          SELECT TAN(RADIANS(LEAST(85.0, GREATEST(-85.0, fa.view_pitch))))
          FROM player_state fa
          WHERE fa.map_id = a_map_id::int AND fa.player_thing_id = a_player_thing_id::int AND fa.view_pitch IS NOT NULL
        ), (
          SELECT v.aimslope
          FROM visible_aim_targets v
          ORDER BY v.priority, v.distance, v.thing_id
          LIMIT 1
        ), 0.0) AS slope
      ),
      pellet_random AS (
        -- P_GunShot: two draws for the spread and one for the damage, per pellet.
        SELECT pellet,
               doom_prandom(i.shot_serial, pellet, 2) AS r1,
               doom_prandom(i.shot_serial, pellet, 3) AS r2,
               i.dmg_dice_mult
                 * (doom_prandom(i.shot_serial, pellet, 4) % i.dmg_dice_count + 1)
                 AS damage
        FROM input i
        CROSS JOIN LATERAL generate_series(0, i.pellet_count - 1) pellet
      ),
      pellets AS (
        SELECT r.pellet, r.damage, a.slope,
               p.view_angle + RADIANS((r.r1-r.r2) * 360.0 / v_spread_units) AS angle
        FROM pellet_random r CROSS JOIN aim a CROSS JOIN shooter p
      ),
      pellet_target_hits AS (
        SELECT b.pellet, b.damage, t.thing_id, t.sector_id, t.no_blood,
               tdist.distance, b.angle, b.slope, 'target'::text AS hit_kind,
               NULL::int AS line_id,NULL::boolean AS line_from_front
        FROM pellets b CROSS JOIN shooter p CROSS JOIN targets t
        CROSS JOIN LATERAL (
          SELECT
            (t.x-p.px)*COS(b.angle)+(t.y-p.py)*SIN(b.angle) AS along,
            POWER(-(t.x-p.px)*SIN(b.angle)+(t.y-p.py)*COS(b.angle), 2) AS perp2
        ) g
        CROSS JOIN LATERAL (
          SELECT g.along-SQRT(GREATEST(0.0,t.radius*t.radius-g.perp2)) AS distance
        ) tdist
        WHERE g.along > 0 AND g.perp2 <= t.radius*t.radius
          AND tdist.distance > 0 AND tdist.distance <= p.max_range
          AND p.shoot_z + b.slope*tdist.distance BETWEEN t.base_z AND t.base_z+t.height
      ),
      pellet_wall_hits AS (
        SELECT b.pellet, b.damage, NULL::int AS thing_id,
               COALESCE(l.right_sector, l.left_sector) AS sector_id,
               TRUE AS no_blood, q.distance, b.angle, b.slope,
               'wall'::text AS hit_kind,l.line_id,
               ((l.x2-l.x1)*(p.py-l.y1)-(l.y2-l.y1)*(p.px-l.x1))<0
                 AS line_from_front
        FROM pellets b CROSS JOIN shooter p CROSS JOIN map_lines l
        CROSS JOIN LATERAL (
          SELECT
            ((l.x1-p.px)*(l.y2-l.y1) - (l.y1-p.py)*(l.x2-l.x1))
              / NULLIF(COS(b.angle)*(l.y2-l.y1)-SIN(b.angle)*(l.x2-l.x1),0.0)
                AS distance,
            ((l.x1-p.px)*SIN(b.angle)-(l.y1-p.py)*COS(b.angle))
              / NULLIF(COS(b.angle)*(l.y2-l.y1)-SIN(b.angle)*(l.x2-l.x1),0.0)
                AS line_u
        ) q
        WHERE q.distance > 0 AND q.distance <= p.max_range AND q.line_u BETWEEN 0 AND 1
          AND (l.right_sector IS NULL OR l.left_sector IS NULL
            OR p.shoot_z+b.slope*q.distance <= l.open_bottom
            OR p.shoot_z+b.slope*q.distance >= l.open_top)
      ),
      nearest_hits AS (
        SELECT * FROM (
          SELECT hits.*,
                 ROW_NUMBER() OVER (PARTITION BY pellet ORDER BY distance,
                   CASE hit_kind WHEN 'target' THEN 0 ELSE 1 END,
                   thing_id, line_id) AS rn
          FROM (
            SELECT * FROM pellet_target_hits
            UNION ALL SELECT * FROM pellet_wall_hits
          ) hits
        ) ranked WHERE rn = 1
      )
        INSERT INTO hitscan_hits
          (map_id,player_thing_id,shot_serial,pellet,damage,thing_id,sector_id,
           no_blood,distance,angle,slope,hit_kind,px,py,shoot_z,line_id,line_from_front)
        SELECT p.map_id,p.player_id,p.shot_serial,h.pellet,h.damage,h.thing_id,
               h.sector_id,h.no_blood,h.distance,h.angle,h.slope,h.hit_kind,
               p.px,p.py,p.shoot_z,h.line_id,h.line_from_front
        FROM nearest_hits h CROSS JOIN shooter p
        ON CONFLICT (map_id,player_thing_id,shot_serial,pellet) DO UPDATE
        SET damage=EXCLUDED.damage,thing_id=EXCLUDED.thing_id,
            sector_id=EXCLUDED.sector_id,no_blood=EXCLUDED.no_blood,
            distance=EXCLUDED.distance,angle=EXCLUDED.angle,slope=EXCLUDED.slope,
            hit_kind=EXCLUDED.hit_kind,px=EXCLUDED.px,py=EXCLUDED.py,
            shoot_z=EXCLUDED.shoot_z,line_id=EXCLUDED.line_id,
            line_from_front=EXCLUDED.line_from_front;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT CAST(COUNT(CASE WHEN h.hit_kind='target' THEN 1 END) AS SIGNED) AS pellets_hit,
             0 AS monsters_killed,
             CAST(COUNT(*) AS SIGNED) AS impacts
      FROM hitscan_hits h JOIN player_weapons pw
        ON pw.map_id = h.map_id AND pw.player_thing_id = h.player_thing_id AND pw.shot_serial = h.shot_serial
      WHERE h.map_id = a_map_id AND h.player_thing_id = a_player_thing_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_pellets_hit, v_monsters_killed, v_impacts;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_impact_count = v_impacts;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_impact_count;
END;;

-- ==== 23_cs_hitscan_apply.sql ====
DROP FUNCTION IF EXISTS doom_cs_hitscan_apply;;
CREATE FUNCTION doom_cs_hitscan_apply(a_map_id INT, a_player_thing_id INT, a_shot_serial BIGINT) RETURNS BOOLEAN
BEGIN
  DECLARE v_dropped_base DOUBLE;
  DECLARE v_special BOOLEAN;
  DECLARE v_unused_0 INT;
  DECLARE v_special_queued BOOLEAN;
  SET v_dropped_base = doom_const('DROPPED_THING_ID_BASE');
  -- Apply and retire one blast's staged traces.
  -- Shared by every hitscan weapon
  UPDATE thing_health h
  SET health = h.health - d.damage,
      alive = h.health - d.damage > 0
  FROM (
    SELECT map_id, thing_id, SUM(damage)::int AS damage
    FROM hitscan_hits
    WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id AND shot_serial=a_shot_serial
      AND hit_kind='target'
    GROUP BY map_id, thing_id
  ) d
  WHERE h.map_id=d.map_id AND h.thing_id=d.thing_id;
  -- Pellets that hit another player. Their health lives in player_state, so the
  -- UPDATE above never sees them; the same P_DamageMobj arithmetic applies:
  -- sk_baby halves it, armour takes its third or half, god mode refuses it
  -- but still takes the shove, and a killing blow names the shooter for
  -- doom_mp_frags. The chainsaw shoves nobody.
  WITH hits AS (
    SELECT hh.thing_id AS victim_id, SUM(hh.damage)::int AS raw,
           MAX(hh.px)::float8 AS sx, MAX(hh.py)::float8 AS sy
    FROM hitscan_hits hh
    JOIN player_state v ON v.map_id=hh.map_id AND v.player_thing_id=hh.thing_id
    WHERE hh.map_id=a_map_id AND hh.player_thing_id=a_player_thing_id
      AND hh.shot_serial=a_shot_serial AND hh.hit_kind='target' AND hh.damage>0
    GROUP BY hh.thing_id
  ),
  scaled AS (
    SELECT h.victim_id, h.sx, h.sy,
           h.raw >> CASE WHEN g.skill=0 THEN 1 ELSE 0 END AS dmg,
           v.armor, v.armor_class, v.god_mode OR v.invuln_tics>0 AS blocked,
           vt.x::float8 AS vx, vt.y::float8 AS vy, pw.current_weapon
    FROM hits h
    JOIN player_state v ON v.map_id=a_map_id AND v.player_thing_id=h.victim_id
    JOIN game_tic_commands g ON g.map_id=v.map_id AND g.player_thing_id=v.player_thing_id
    JOIN things vt ON vt.map_id=v.map_id AND vt.id=v.player_thing_id
    JOIN player_weapons pw ON pw.map_id=a_map_id AND pw.player_thing_id=a_player_thing_id
  ),
  applied AS (
    SELECT s.*,
           CASE WHEN s.blocked THEN 0
                ELSE LEAST(s.armor, CASE s.armor_class WHEN 2 THEN FLOOR(s.dmg/2.0)::int
                                                       WHEN 1 THEN FLOOR(s.dmg/3.0)::int
                                                       ELSE 0 END) END AS saved,
           CASE WHEN s.current_weapon=8 OR s.dist=0 THEN 0.0
                ELSE s.dmg*0.125*(s.vx-s.sx)/s.dist END AS thrust_x,
           CASE WHEN s.current_weapon=8 OR s.dist=0 THEN 0.0
                ELSE s.dmg*0.125*(s.vy-s.sy)/s.dist END AS thrust_y
    FROM (SELECT sc.*, SQRT(POWER(sc.vx-sc.sx,2)+POWER(sc.vy-sc.sy,2)) AS dist
          FROM scaled sc) s
  )
  UPDATE player_state ps
  SET armor = ps.armor - a.saved,
      armor_class = CASE WHEN ps.armor-a.saved<=0 THEN 0 ELSE ps.armor_class END,
      health = GREATEST(0, ps.health - CASE WHEN a.blocked THEN 0 ELSE a.dmg-a.saved END),
      damage_count = LEAST(100, ps.damage_count + CASE WHEN a.blocked THEN 0 ELSE a.dmg-a.saved END),
      alive = GREATEST(0, ps.health - CASE WHEN a.blocked THEN 0 ELSE a.dmg-a.saved END) > 0,
      pain_face_tics = CASE WHEN NOT a.blocked AND a.dmg-a.saved > 0 THEN 12 ELSE ps.pain_face_tics END,
      killer_id = CASE WHEN ps.alive AND NOT a.blocked
                            AND ps.health - (a.dmg-a.saved) <= 0
                       THEN a_player_thing_id::int ELSE ps.killer_id END,
      momentum_x = (ps.momentum_x + a.thrust_x)::real,
      momentum_y = (ps.momentum_y + a.thrust_y)::real
  FROM applied a
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a.victim_id;
  -- P_DamageMobj's thrust, for bullets. P_LineAttack passes the shooter as the
  -- inflictor, so the shove is along the line from the player to the target:
  -- `thrust = damage*12.5/mass` map units a tic.
  --
  -- Except with the chainsaw. Vanilla skips the thrust when the source is a
  -- player holding one -- `source->player->readyweapon != wp_chainsaw` -- which
  -- is exactly why a chainsaw holds an enemy in place instead of pushing it out
  -- of reach while it grinds.
  UPDATE things t
  SET mom_x = (t.mom_x + s.push * s.dx / s.dist)::real,
      mom_y = (t.mom_y + s.push * s.dy / s.dist)::real
  FROM (
    SELECT hh.thing_id,
           SUM(hh.damage) * 12.5 / GREATEST(1, cd.mass) AS push,
           (v.x - pt.x)::float8 AS dx, (v.y - pt.y)::float8 AS dy,
           NULLIF(SQRT(POWER(v.x-pt.x,2)+POWER(v.y-pt.y,2)),0)::float8 AS dist
    FROM hitscan_hits hh
    JOIN things v ON v.map_id=hh.map_id AND v.id=hh.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=v.type
    JOIN things pt ON pt.map_id=hh.map_id AND pt.id=a_player_thing_id::int
    JOIN player_weapons pw ON pw.map_id=hh.map_id
     AND pw.player_thing_id=a_player_thing_id::int
    WHERE hh.map_id=a_map_id AND hh.player_thing_id=a_player_thing_id
      AND hh.shot_serial=a_shot_serial AND hh.hit_kind='target' AND hh.damage>0
      AND pw.current_weapon <> 8
      -- A player victim's shove goes to player_state (below), not their Thing.
      AND NOT EXISTS (SELECT 1 FROM player_state ps
                      WHERE ps.map_id=hh.map_id AND ps.player_thing_id=hh.thing_id)
    GROUP BY hh.thing_id, cd.mass, v.x, v.y, pt.x, pt.y
  ) s
  WHERE t.map_id=a_map_id AND t.id=s.thing_id AND s.dist IS NOT NULL;
  -- Roll pain independently for every damaging pellet.
  -- Any successful roll puts a surviving actor into its pain state. Getting shot still
  -- alerts an actor even when the pain roll fails
  WITH pain_rolls AS (
    SELECT hh.map_id,hh.thing_id,t.type,
           BOOL_OR(doom_prandom(hh.thing_id, a_shot_serial, 1)
                     < cd.pain_chance) AS flinches
    FROM hitscan_hits hh
    JOIN things t ON t.map_id=hh.map_id AND t.id=hh.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type
    WHERE hh.map_id=a_map_id AND hh.player_thing_id=a_player_thing_id AND hh.shot_serial=a_shot_serial
      AND hh.hit_kind='target' AND hh.damage>0
    GROUP BY hh.map_id,hh.thing_id,t.type
  )
  UPDATE monster_ai ai
  SET state=CASE WHEN hit.flinches THEN 'pain' ELSE 'see' END,
      seq_index=CASE WHEN hit.flinches THEN 0 ELSE ai.seq_index END,
      state_tics=CASE WHEN hit.flinches THEN COALESCE(fp.tics,ai.state_tics)
                      ELSE COALESCE(fs.tics,ai.state_tics) END
  FROM pain_rolls hit
  JOIN thing_health h ON h.map_id = hit.map_id AND h.thing_id = hit.thing_id
  LEFT JOIN thing_ai_frames fp ON fp.thing_type=hit.type
    AND fp.state='pain' AND fp.seq_index=0
  LEFT JOIN thing_ai_frames fs ON fs.thing_type=hit.type
    AND fs.state='see' AND fs.seq_index=0
  WHERE ai.map_id = hit.map_id AND ai.thing_id = hit.thing_id
    AND h.alive AND ai.state NOT IN ('die','dead')
    AND NOT EXISTS (SELECT 1 FROM thing_combat_defs bd
                    WHERE bd.thing_type = hit.type AND bd.explodes)
    -- A failed pain roll still alerts a dormant monster, but must not reset an
    -- actor that is already chasing or attacking.
    AND (hit.flinches OR ai.state='stand');
  INSERT INTO world_effects
    (map_id,effect_id,effect_type,x,y,z,sector_id,age)
  SELECT map_id,shot_serial*16+pellet,
         CASE WHEN hit_kind='target' AND NOT no_blood THEN 'blood' ELSE 'puff' END,
         px+GREATEST(0.0,distance-CASE WHEN hit_kind='target' THEN 10 ELSE 4 END)*COS(angle),
         py+GREATEST(0.0,distance-CASE WHEN hit_kind='target' THEN 10 ELSE 4 END)*SIN(angle),
         shoot_z+slope*GREATEST(0.0,distance-CASE WHEN hit_kind='target' THEN 10 ELSE 4 END),
         sector_id,0
  FROM hitscan_hits
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id AND shot_serial=a_shot_serial
  ON CONFLICT (map_id,effect_id) DO UPDATE
  SET effect_type=EXCLUDED.effect_type,x=EXCLUDED.x,y=EXCLUDED.y,z=EXCLUDED.z,
      sector_id=EXCLUDED.sector_id,age=0;
  -- Vanilla only has a couple of monsters drop anything on death (everything
  -- else, including the imp, drops nothing): zombieman -> a clip, sergeant -> a shotgun.
  WITH just_died AS (
    SELECT DISTINCT hh.map_id, hh.thing_id
    FROM hitscan_hits hh
    JOIN thing_health th ON th.map_id = hh.map_id AND th.thing_id = hh.thing_id
    WHERE hh.map_id = a_map_id AND hh.player_thing_id = a_player_thing_id AND hh.shot_serial = a_shot_serial
      AND hh.hit_kind = 'target' AND NOT th.alive
  ),
  drops AS (
    SELECT jd.map_id, jd.thing_id AS source_thing_id, t.x, t.y, t.angle,
           cd.drops_thing_type AS drop_type
    FROM just_died jd
    JOIN things t ON t.map_id = jd.map_id AND t.id = jd.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type = t.type
     AND cd.drops_thing_type IS NOT NULL
  )
  -- Dropped-item Things get a deterministic id well above any real WAD thing
  INSERT INTO things (map_id, id, x, y, angle, spawn_x, spawn_y, spawn_angle, type, flags)
  SELECT map_id, v_dropped_base + source_thing_id, x, y, angle, x, y, angle, drop_type, 7
  FROM drops
  ON CONFLICT (map_id, id) DO NOTHING;
  WITH just_died AS (
    SELECT DISTINCT hh.map_id, hh.thing_id
    FROM hitscan_hits hh
    JOIN thing_health th ON th.map_id = hh.map_id AND th.thing_id = hh.thing_id
    WHERE hh.map_id = a_map_id AND hh.player_thing_id = a_player_thing_id AND hh.shot_serial = a_shot_serial
      AND hh.hit_kind = 'target' AND NOT th.alive
  ),
  drops AS (
    SELECT jd.map_id, jd.thing_id AS source_thing_id,
           COALESCE(ai.sector_id, rt.sector_id) AS sector_id,
           cd.drops_thing_type AS drop_type
    FROM just_died jd
    JOIN things t ON t.map_id = jd.map_id AND t.id = jd.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type = t.type
     AND cd.drops_thing_type IS NOT NULL
    LEFT JOIN monster_ai ai ON ai.map_id = jd.map_id AND ai.thing_id = jd.thing_id
    LEFT JOIN render_things rt ON rt.map_id = jd.map_id AND rt.thing_id = jd.thing_id
  )
  INSERT INTO render_things
    (map_id, thing_id, sector_id, sprite, frame, fullbright, spawn_ceiling, thing_height)
  SELECT d.map_id, v_dropped_base + d.source_thing_id, d.sector_id,
         ts.sprite, ts.frame, ts.fullbright, ts.spawn_ceiling, ts.thing_height
  FROM drops d
  JOIN thing_sprite_defs ts ON ts.thing_type = d.drop_type
  WHERE d.sector_id IS NOT NULL
  ON CONFLICT (map_id, thing_id) DO NOTHING;
  -- Gun-activated linedefs, Doom's G1/GR.
  INSERT INTO line_special_events
    (map_id,player_thing_id,line_id,trigger_type,from_front)
  SELECT DISTINCT hh.map_id,hh.player_thing_id,hh.line_id,'shoot',
         COALESCE(hh.line_from_front,TRUE)
  FROM hitscan_hits hh
  JOIN linedefs ld ON ld.map_id=hh.map_id AND ld.id=hh.line_id
  JOIN line_special_defs d ON d.special=ld.special AND d.shoot_activated
  WHERE hh.map_id=a_map_id AND hh.player_thing_id=a_player_thing_id
    AND hh.shot_serial=a_shot_serial
    AND hh.hit_kind='wall'
  ON CONFLICT (map_id,player_thing_id,line_id,trigger_type) DO NOTHING;
  DELETE FROM hitscan_hits
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id AND shot_serial=a_shot_serial;
  SET v_special = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT 0::int,
             EXISTS (SELECT 1 FROM line_special_events e
                     WHERE e.map_id=a_map_id AND e.player_thing_id=a_player_thing_id
                       AND e.trigger_type='shoot') AS special_queued;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_unused_0, v_special_queued;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_special = v_special_queued;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_special;
END;;

-- ==== 24_cs_projectiles.sql ====
DROP FUNCTION IF EXISTS doom_cs_projectiles;;
CREATE FUNCTION doom_cs_projectiles(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_player_radius DOUBLE;
  DECLARE v_player_height DOUBLE;
  DECLARE v_view_height DOUBLE;
  DECLARE v_missile_spawn_z DOUBLE;
  DECLARE v_aim_spread DOUBLE;
  DECLARE v_shadow_units DOUBLE;
  DECLARE v_effect_id_base DOUBLE;
  DECLARE v_spawn_ahead DOUBLE;
  DECLARE v_nightmare_missile_speed DOUBLE;
  DECLARE v_melee_reach DOUBLE;
  DECLARE v_has_missiles BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_has_missiles = false;
  BEGIN
    DECLARE b_done INT DEFAULT 0;
    DECLARE b_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM monster_projectiles WHERE map_id=a_map_id)
                                  OR EXISTS (SELECT 1 FROM monster_ai WHERE map_id=a_map_id AND fired_this_tick)
                                  OR EXISTS (SELECT 1 FROM player_weapons WHERE map_id=a_map_id AND fired_this_tick AND current_weapon IN (5,6,7)) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b_done = 1;
    OPEN b_rows;
    b_each: LOOP
      FETCH b_rows INTO v_e;
      IF b_done THEN LEAVE b_each; END IF;
      SET v_has_missiles = v_e;
    END LOOP;
    CLOSE b_rows;
  END;
  IF NOT v_has_missiles THEN
    DELETE FROM projectile_damage WHERE map_id=a_map_id;
    DELETE FROM projectile_impacts WHERE map_id=a_map_id;
    RETURN 0;
  END IF;
  SET v_player_radius = doom_const('PLAYER_RADIUS');
  SET v_player_height = doom_const('PLAYER_HEIGHT');
  SET v_view_height = doom_const('VIEWHEIGHT');
  SET v_missile_spawn_z = doom_const('MISSILE_SPAWN_Z');
  SET v_aim_spread = doom_const('AIM_SPREAD_DEGREES');
  SET v_shadow_units = doom_const('SHADOW_SPREAD_UNITS');
  SET v_effect_id_base = doom_const('PROJECTILE_EFFECT_ID_BASE');
  SET v_spawn_ahead = doom_const('MISSILE_SPAWN_AHEAD');
  SET v_nightmare_missile_speed = doom_const('NIGHTMARE_MISSILE_SPEED');
  SET v_melee_reach = doom_const('MELEE_REACH');
  -- Advance every Doom missile.
  DELETE FROM projectile_damage WHERE map_id=a_map_id;
  DELETE FROM projectile_impacts WHERE map_id=a_map_id;
  -- spawn imp fireballs
  WITH
  params AS (SELECT a_map_id::int AS map_id,a_player_thing_id::int AS player_thing_id),
  source AS (
    SELECT ai.thing_id,t.x::double precision AS sx,t.y::double precision AS sy,
           (CASE WHEN d.floats THEN t.z ELSE s.floor_height END
            + v_missile_spawn_z)::double precision AS sz,
           pt.x::double precision AS tx,pt.y::double precision AS ty,
           (pt.z - (v_view_height - v_player_height/2.0))::double precision AS tz,
           COALESCE(ai.sector_id,rt.sector_id) AS sector_id,
           d.missile_type,d.missile_speed,d.missile_dice,pd.dmg_dice_count,
           (d.melee_mult IS NOT NULL) AS has_melee
    FROM monster_ai ai JOIN params p ON ai.map_id=p.map_id
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_combat_defs d ON d.thing_type=t.type AND d.missile_type IS NOT NULL
    JOIN projectile_defs pd ON pd.projectile_type=d.missile_type
    JOIN things pt ON pt.map_id=p.map_id AND pt.id=COALESCE(ai.target_thing_id,p.player_thing_id)
    LEFT JOIN render_things rt ON rt.map_id=ai.map_id AND rt.thing_id=ai.thing_id
    JOIN sectors s ON s.map_id=ai.map_id
      AND s.id=COALESCE(ai.sector_id,rt.sector_id)
    WHERE ai.fired_this_tick
      -- in melee range a thing with a bite bites instead (A_TroopAttack and
      -- friends check P_CheckMeleeRange first); one without fires regardless
      AND (NOT d.melee_mult IS NOT NULL OR SQRT(POWER(pt.x-t.x,2)+POWER(pt.y-t.y,2))>v_melee_reach)
  ),
  -- MF_SHADOW on the target, which for the player means the blur sphere.
  shadow AS (
    SELECT (ps.invis_tics > 0) AS shadowed, ps.level_tics
    FROM player_state ps JOIN params p ON ps.map_id=p.map_id
     AND ps.player_thing_id=p.player_thing_id
  ),
  velocity AS (
    -- P_SpawnMissile aims at where the target is, then, if the target carries
    -- MF_SHADOW -- the blur sphere -- throws the shot off by
    -- (P_Random()-P_Random())<<20, which is up to about 22 degrees either way.
    -- Rotating the aim vector is the same thing as skewing the angle.
    SELECT s.*,
           (s.tx-s.sx)*COS(sk.err)-(s.ty-s.sy)*SIN(sk.err) AS dx,
           (s.tx-s.sx)*SIN(sk.err)+(s.ty-s.sy)*COS(sk.err) AS dy,
           (s.tz-s.sz) AS dz,
           SQRT(POWER(s.tx-s.sx,2)+POWER(s.ty-s.sy,2)) AS horizontal_len
    FROM source s
    CROSS JOIN LATERAL (
      SELECT CASE WHEN sh.shadowed
                  THEN RADIANS((doom_prandom(s.thing_id, sh.level_tics, 5)
                               -doom_prandom(s.thing_id, sh.level_tics, 6))
                               *360.0/v_shadow_units)
                  ELSE 0.0 END AS err
      FROM shadow sh
    ) sk
  ),
  numbered AS (
    SELECT v.*,(SELECT sh.level_tics FROM shadow sh) AS level_tics,
           ROW_NUMBER() OVER (ORDER BY thing_id) AS rn,
           COALESCE((SELECT MAX(projectile_id) FROM monster_projectiles mp
                     WHERE mp.map_id=a_map_id),0) AS max_id
    FROM velocity v
  )
  INSERT INTO monster_projectiles
    (map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,
     sector_id,state,age,damage,impact_player)
  SELECT p.map_id,n.max_id+n.rn,n.thing_id,
         n.missile_type,
         n.sx+v_spawn_ahead*n.dx/NULLIF(n.horizontal_len,0),
         n.sy+v_spawn_ahead*n.dy/NULLIF(n.horizontal_len,0),n.sz,
         -- BAL7 travels at 15 map units a tic, BAL1 at 10. G_InitNew raises
         -- both to 20 on Nightmare, which is the whole of "fast missiles".
         (CASE WHEN (SELECT g.skill FROM game_tic_commands g
                       WHERE g.map_id=a_map_id::int
                         AND g.player_thing_id=a_player_thing_id::int)=4
                THEN v_nightmare_missile_speed
                ELSE n.missile_speed END)*n.dx/NULLIF(n.horizontal_len,0),
         (CASE WHEN (SELECT g.skill FROM game_tic_commands g
                       WHERE g.map_id=a_map_id::int
                         AND g.player_thing_id=a_player_thing_id::int)=4
                THEN v_nightmare_missile_speed
                ELSE n.missile_speed END)*n.dy/NULLIF(n.horizontal_len,0),
         (CASE WHEN (SELECT g.skill FROM game_tic_commands g
                       WHERE g.map_id=a_map_id::int
                         AND g.player_thing_id=a_player_thing_id::int)=4
                THEN v_nightmare_missile_speed
                ELSE n.missile_speed END)*n.dz/NULLIF(n.horizontal_len,0),
         n.sector_id,'fly',0,
         -- A_TroopAttack rolls 3d8, A_BruisAttack 8d8.
         (n.missile_dice
          *(doom_prandom(n.thing_id, n.level_tics, 7) % n.dmg_dice_count + 1))::smallint,FALSE
  FROM numbered n CROSS JOIN params p
  WHERE n.horizontal_len>0
  ON CONFLICT (map_id, projectile_id) DO UPDATE SET
    owner_thing_id=EXCLUDED.owner_thing_id, projectile_type=EXCLUDED.projectile_type,
    x=EXCLUDED.x, y=EXCLUDED.y, z=EXCLUDED.z, vx=EXCLUDED.vx, vy=EXCLUDED.vy, vz=EXCLUDED.vz,
    sector_id=EXCLUDED.sector_id, state=EXCLUDED.state, age=EXCLUDED.age, damage=EXCLUDED.damage,
    impact_player=EXCLUDED.impact_player;
  -- spawn player rockets/plasma/BFG
  WITH
  params AS (SELECT a_map_id::int AS map_id,a_player_thing_id::int AS player_thing_id),
  source AS (
    SELECT p.map_id,pw.player_thing_id,t.x::double precision AS sx,
           t.y::double precision AS sy,
           (t.z - (v_view_height - v_missile_spawn_z))::double precision AS sz,
           RADIANS(t.angle::double precision) AS angle,
           wd.projectile_type,
           pd.speed::double precision AS speed,
           -- free aim (the modern client's mouse look): the missile flies along the view's slope
           CASE WHEN fa.view_pitch IS NULL THEN NULL
                ELSE TAN(RADIANS(LEAST(85.0, GREATEST(-85.0, fa.view_pitch)))) END AS free_slope,
           -- A_FireMissile 20d8, A_FirePlasma 5d8, A_FireBFG 100d8, each keyed
           -- on the shot that fired it and on its own call site.
           pd.dmg_dice_mult
             * (doom_prandom(pw.shot_serial,pw.player_thing_id,pd.dmg_random_id)
                % pd.dmg_dice_count + 1)
             AS damage,
           rt.sector_id
    FROM params p
    -- Every player who fired a launcher this tic
    JOIN player_weapons pw ON pw.map_id=p.map_id
    JOIN weapon_defs wd ON wd.weapon_id=pw.current_weapon
     AND wd.projectile_type IS NOT NULL
    JOIN projectile_defs pd ON pd.projectile_type=wd.projectile_type
    JOIN things t ON t.map_id=pw.map_id AND t.id=pw.player_thing_id
    LEFT JOIN render_things rt ON rt.map_id=t.map_id AND rt.thing_id=t.id
    LEFT JOIN player_state fa ON fa.map_id=pw.map_id AND fa.player_thing_id=pw.player_thing_id
    WHERE pw.fired_this_tick
  ),
  targets AS (
    SELECT s.*,mt.id AS target_id,mt.x::double precision AS tx,
           mt.y::double precision AS ty,
           (sec.floor_height+rt.thing_height/2.0)::double precision AS tz,
           cd.radius::double precision AS radius
    FROM source s
    JOIN thing_health h ON h.map_id=s.map_id AND h.alive
    JOIN things mt ON mt.map_id=h.map_id AND mt.id=h.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=mt.type
    JOIN render_things rt ON rt.map_id=mt.map_id AND rt.thing_id=mt.id
    LEFT JOIN monster_ai ai ON ai.map_id=mt.map_id AND ai.thing_id=mt.id
    JOIN sectors sec ON sec.map_id=mt.map_id
      AND sec.id=COALESCE(ai.sector_id,rt.sector_id)
  ),
  target_geometry AS (
    SELECT t.*,
           (t.tx-t.sx)*COS(t.angle)+(t.ty-t.sy)*SIN(t.angle) AS along,
           -(t.tx-t.sx)*SIN(t.angle)+(t.ty-t.sy)*COS(t.angle) AS lateral_offset
    FROM targets t
  ),
  ranked_targets AS (
    SELECT g.*,ROW_NUMBER() OVER (PARTITION BY map_id,player_thing_id
      ORDER BY ABS(lateral_offset)/NULLIF(along,0),along,target_id) AS rn
    FROM target_geometry g
    WHERE along>0 AND ABS(lateral_offset)<=radius+along*TAN(RADIANS(v_aim_spread))
  ),
  aimed AS (
    SELECT s.*,r.tz,r.along
    FROM source s LEFT JOIN ranked_targets r
      ON r.map_id=s.map_id AND r.player_thing_id=s.player_thing_id AND r.rn=1
  ),
  numbered AS (
    SELECT a.*,COALESCE((SELECT MAX(projectile_id)
           FROM monster_projectiles mp WHERE mp.map_id=a_map_id),0)
           +ROW_NUMBER() OVER (ORDER BY player_thing_id) AS projectile_id
    FROM aimed a
  )
  INSERT INTO monster_projectiles
    (map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,
     sector_id,state,age,damage,impact_player)
  SELECT map_id,projectile_id,player_thing_id,projectile_type,sx,sy,sz,
         speed*COS(angle),speed*SIN(angle),
         speed*COALESCE(free_slope,(tz-sz)/NULLIF(along,0),0.0),
         sector_id,'fly',0,damage::smallint,FALSE
  FROM numbered
  ON CONFLICT (map_id, projectile_id) DO UPDATE SET
    owner_thing_id=EXCLUDED.owner_thing_id, projectile_type=EXCLUDED.projectile_type,
    x=EXCLUDED.x, y=EXCLUDED.y, z=EXCLUDED.z, vx=EXCLUDED.vx, vy=EXCLUDED.vy, vz=EXCLUDED.vz,
    sector_id=EXCLUDED.sector_id, state=EXCLUDED.state, age=EXCLUDED.age, damage=EXCLUDED.damage,
    impact_player=EXCLUDED.impact_player;
  -- swept collision against actors/player/walls
  WITH
  params AS (SELECT a_map_id::int AS map_id,a_player_thing_id::int AS player_thing_id),
  flying AS (
    SELECT mp.*, pd.radius::double precision AS radius
    FROM monster_projectiles mp JOIN params p ON mp.map_id=p.map_id
    JOIN projectile_defs pd ON pd.projectile_type=mp.projectile_type
    WHERE mp.state='fly'
  ),
  players AS (
    -- Every live player is a target for every missile except its own owner's
    SELECT ps.player_thing_id,t.x::double precision AS x,
           t.y::double precision AS y,
           (ps.base_z-v_view_height)::double precision AS base_z
    FROM player_state ps JOIN params p ON ps.map_id=p.map_id
    JOIN things t ON t.map_id=ps.map_id AND t.id=ps.player_thing_id
    WHERE ps.alive
  ),
  player_hits AS (
    SELECT f.projectile_id,p.player_thing_id,q.t
    FROM flying f JOIN players p ON p.player_thing_id<>f.owner_thing_id
    CROSS JOIN LATERAL (
      SELECT LEAST(1.0,GREATEST(0.0,
        ((p.x-f.x)*f.vx+(p.y-f.y)*f.vy)
         /NULLIF(f.vx*f.vx+f.vy*f.vy,0.0))) AS t
    ) q
    WHERE POWER(f.x+q.t*f.vx-p.x,2)+POWER(f.y+q.t*f.vy-p.y,2)
            <= POWER(f.radius+v_player_radius,2)
      AND f.z+q.t*f.vz BETWEEN p.base_z AND p.base_z+v_player_height
  ),
  actor_targets AS (
    SELECT h.map_id,h.thing_id,t.type AS thing_type,t.x::double precision AS x,
           t.y::double precision AS y,cd.radius::double precision AS radius,
           sec.floor_height::double precision AS base_z,
           rt.thing_height::double precision AS height
    FROM thing_health h JOIN params p ON h.map_id=p.map_id AND h.alive
    JOIN things t ON t.map_id=h.map_id AND t.id=h.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type
    JOIN render_things rt ON rt.map_id=t.map_id AND rt.thing_id=t.id
    LEFT JOIN monster_ai ai ON ai.map_id=t.map_id AND ai.thing_id=t.id
    JOIN sectors sec ON sec.map_id=t.map_id
      AND sec.id=COALESCE(ai.sector_id,rt.sector_id)
  ),
  actor_hit_candidates AS (
    SELECT f.projectile_id,t.thing_id,q.t
    FROM flying f JOIN actor_targets t ON t.map_id=f.map_id
    CROSS JOIN LATERAL (
      SELECT LEAST(1.0,GREATEST(0.0,
        ((t.x-f.x)*f.vx+(t.y-f.y)*f.vy)
         /NULLIF(f.vx*f.vx+f.vy*f.vy,0.0))) AS t
    ) q
    -- PIT_CheckThing refuses to damage the shooter's own species: a missile
    -- passing through one of its own kind explodes and does nothing.
    LEFT JOIN things ot ON ot.map_id=f.map_id AND ot.id=f.owner_thing_id
    WHERE (f.projectile_type IN ('rocket','plasma','bfg')
           OR (f.projectile_type IN ('imp_fireball','baron_fireball','caco_fireball')
               AND ot.type IS DISTINCT FROM t.thing_type))
      AND t.thing_id<>f.owner_thing_id
      AND POWER(f.x+q.t*f.vx-t.x,2)+POWER(f.y+q.t*f.vy-t.y,2)
            <= POWER(f.radius+t.radius,2)
      AND f.z+q.t*f.vz BETWEEN t.base_z AND t.base_z+t.height
  ),
  actor_hits AS (
    SELECT projectile_id,thing_id,t FROM (
      SELECT c.*,ROW_NUMBER() OVER
        (PARTITION BY projectile_id ORDER BY t, thing_id) AS rn
      FROM actor_hit_candidates c
    ) r WHERE rn=1
  ),
  wall_hit_candidates AS (
    SELECT f.projectile_id,hit.t
    FROM flying f
    JOIN linedef_geom ld ON ld.map_id=f.map_id
    LEFT JOIN sectors fr ON fr.map_id=ld.map_id AND fr.id=ld.fsec
    LEFT JOIN sectors bk ON bk.map_id=ld.map_id AND bk.id=ld.bsec
    CROSS JOIN LATERAL (
      SELECT f.vx*(ld.y2-ld.y1)-f.vy*(ld.x2-ld.x1) AS denom
    ) d
    CROSS JOIN LATERAL (
      SELECT ((ld.x1-f.x)*(ld.y2-ld.y1)-(ld.y1-f.y)*(ld.x2-ld.x1))
               /NULLIF(d.denom,0.0) AS t,
             ((ld.x1-f.x)*f.vy-(ld.y1-f.y)*f.vx)
               /NULLIF(d.denom,0.0) AS u
    ) hit
    WHERE ABS(d.denom)>1e-9 AND hit.t BETWEEN 0.0 AND 1.0
      AND hit.u BETWEEN 0.0 AND 1.0
      AND (ld.left_sd_id=-1 OR ld.right_sd_id=-1 OR fr.id IS NULL OR bk.id IS NULL
        OR f.z+hit.t*f.vz<=GREATEST(fr.floor_height,bk.floor_height)
        OR f.z+hit.t*f.vz>=LEAST(fr.ceil_height,bk.ceil_height))
  ),
  wall_hits AS (
    SELECT projectile_id,MIN(t) AS t FROM wall_hit_candidates GROUP BY projectile_id
  ),
  candidates AS (
    SELECT projectile_id,t,TRUE AS target_player,player_thing_id AS target_thing_id,0 AS priority
      FROM player_hits
    UNION ALL SELECT projectile_id,t,FALSE,thing_id,0 FROM actor_hits
    UNION ALL SELECT projectile_id,t,FALSE,NULL::int,1 FROM wall_hits
  ),
  nearest AS (
    SELECT * FROM (
      SELECT c.*,ROW_NUMBER() OVER
        (PARTITION BY projectile_id ORDER BY t,priority,
         COALESCE(target_thing_id,-1)) AS rn
      FROM candidates c
    ) r WHERE rn=1
  )
  INSERT INTO projectile_impacts
    (map_id,projectile_id,projectile_type,owner_thing_id,x,y,z,
     target_player,target_thing_id,target_player_thing_id)
  SELECT f.map_id,f.projectile_id,f.projectile_type,f.owner_thing_id,
         f.x+n.t*f.vx,f.y+n.t*f.vy,f.z+n.t*f.vz,
         n.target_player,n.target_thing_id,
         CASE WHEN n.target_player THEN n.target_thing_id END
  FROM nearest n JOIN flying f ON f.projectile_id=n.projectile_id
  ON CONFLICT (map_id, projectile_id) DO NOTHING;
  -- direct player-projectile damage
  INSERT INTO projectile_damage
    (map_id,projectile_id,damage_kind,hit_index,thing_id,damage)
  SELECT i.map_id,i.projectile_id,'direct',-1,i.target_thing_id,mp.damage
  FROM projectile_impacts i
  JOIN monster_projectiles mp ON mp.map_id=i.map_id
    AND mp.projectile_id=i.projectile_id
  WHERE i.map_id=a_map_id::int
    AND i.target_thing_id IS NOT NULL
    AND i.projectile_type IN ('rocket','plasma','bfg',
                              'imp_fireball','baron_fireball','caco_fireball')
  ON CONFLICT (map_id, projectile_id, damage_kind, hit_index) DO NOTHING;
  -- rocket radius damage
  WITH
  params AS (SELECT a_map_id::int AS map_id),
  -- Anything the catalogue gives a blast radius; only the rocket has one.
  explosions AS (
    SELECT i.* FROM projectile_impacts i JOIN params p ON i.map_id=p.map_id
    JOIN projectile_defs pd ON pd.projectile_type=i.projectile_type
     AND pd.blast_radius IS NOT NULL
  ),
  victims AS (
    SELECT h.map_id,h.thing_id,t.x::double precision AS x,t.y::double precision AS y,
           cd.radius::double precision AS radius
    FROM thing_health h JOIN params p ON h.map_id=p.map_id AND h.alive
    JOIN things t ON t.map_id=h.map_id AND t.id=h.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type
    UNION ALL
    -- P_RadiusAttack spares nobody, the shooter included.
    SELECT ps.map_id,ps.player_thing_id,t.x::double precision,t.y::double precision,
           v_player_radius::double precision
    FROM player_state ps JOIN params p ON ps.map_id=p.map_id
    JOIN things t ON t.map_id=ps.map_id AND t.id=ps.player_thing_id
    WHERE ps.alive
  ),
  blocking AS (
    SELECT ld.x1::double precision AS x1,ld.y1::double precision AS y1,
           ld.x2::double precision AS x2,ld.y2::double precision AS y2
    FROM linedef_geom ld JOIN params p ON ld.map_id=p.map_id
    LEFT JOIN sectors fr ON fr.map_id=ld.map_id AND fr.id=ld.fsec
    LEFT JOIN sectors bk ON bk.map_id=ld.map_id AND bk.id=ld.bsec
    WHERE ld.left_sd_id=-1 OR ld.right_sd_id=-1 OR fr.id IS NULL OR bk.id IS NULL
       OR LEAST(fr.ceil_height,bk.ceil_height)<=GREATEST(fr.floor_height,bk.floor_height)
  ),
  radial AS (
    SELECT e.map_id,e.projectile_id,v.thing_id,
           GREATEST(0,pd.blast_radius-GREATEST(0,
             FLOOR(GREATEST(ABS(v.x-e.x),ABS(v.y-e.y))-v.radius)::int)) AS damage
    FROM explosions e CROSS JOIN victims v
    JOIN projectile_defs pd ON pd.projectile_type=e.projectile_type
     AND pd.blast_radius IS NOT NULL
    WHERE GREATEST(ABS(v.x-e.x),ABS(v.y-e.y))-v.radius<pd.blast_radius
      AND NOT EXISTS (
        SELECT 1 FROM blocking b CROSS JOIN LATERAL (
          SELECT (e.x-b.x1)*(b.y2-b.y1)-(e.y-b.y1)*(b.x2-b.x1) AS d1,
                 (v.x-b.x1)*(b.y2-b.y1)-(v.y-b.y1)*(b.x2-b.x1) AS d2,
                 (b.x1-e.x)*(v.y-e.y)-(b.y1-e.y)*(v.x-e.x) AS d3,
                 (b.x2-e.x)*(v.y-e.y)-(b.y2-e.y)*(v.x-e.x) AS d4
        ) q WHERE q.d1*q.d2<0 AND q.d3*q.d4<0
      )
  )
  INSERT INTO projectile_damage
    (map_id,projectile_id,damage_kind,hit_index,thing_id,damage)
  SELECT map_id,projectile_id,'splash',thing_id,thing_id,damage
  FROM radial WHERE damage>0
  ON CONFLICT (map_id, projectile_id, damage_kind, hit_index) DO NOTHING;
  -- BFG spray, 40 rays across 90 degrees
  WITH
  params AS (SELECT a_map_id::int AS map_id),
  bfg AS (
    SELECT mp.*,t.x::double precision AS ox,t.y::double precision AS oy,
           pd.spray_rays,pd.spray_arc_degrees,pd.spray_range,
           pd.spray_dice,pd.spray_random_base,pd.dmg_dice_count
    FROM monster_projectiles mp JOIN params p ON mp.map_id=p.map_id
    JOIN things t ON t.map_id=mp.map_id AND t.id=mp.owner_thing_id
    JOIN projectile_defs pd ON pd.projectile_type=mp.projectile_type
    WHERE mp.state='explode' AND pd.spray_at_tic IS NOT NULL
      AND mp.age=pd.spray_at_tic
  ),
  -- A_BFGSpray fans spray_rays evenly across spray_arc_degrees, centred on the
  -- ball's own heading.
  rays AS (
    SELECT b.*,r.i AS ray_index,
           ATAN2(b.vy,b.vx)
             + RADIANS(-b.spray_arc_degrees/2.0
                       + (b.spray_arc_degrees/b.spray_rays)*r.i) AS ray_angle
    FROM bfg b CROSS JOIN LATERAL generate_series(0,b.spray_rays-1) r(i)
  ),
  targets AS (
    SELECT h.map_id,h.thing_id,t.x::double precision AS x,t.y::double precision AS y,
           cd.radius::double precision AS radius
    FROM thing_health h JOIN params p ON h.map_id=p.map_id AND h.alive
    JOIN things t ON t.map_id=h.map_id AND t.id=h.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type
    UNION ALL
    -- The spray reaches players too; the shooter is excluded below.
    SELECT ps.map_id,ps.player_thing_id,t.x::double precision,t.y::double precision,
           v_player_radius::double precision
    FROM player_state ps JOIN params p ON ps.map_id=p.map_id
    JOIN things t ON t.map_id=ps.map_id AND t.id=ps.player_thing_id
    WHERE ps.alive
  ),
  blocking AS (
    SELECT ld.x1::double precision AS x1,ld.y1::double precision AS y1,
           ld.x2::double precision AS x2,ld.y2::double precision AS y2
    FROM linedef_geom ld JOIN params p ON ld.map_id=p.map_id
    LEFT JOIN sectors fr ON fr.map_id=ld.map_id AND fr.id=ld.fsec
    LEFT JOIN sectors bk ON bk.map_id=ld.map_id AND bk.id=ld.bsec
    WHERE ld.left_sd_id=-1 OR ld.right_sd_id=-1 OR fr.id IS NULL OR bk.id IS NULL
       OR LEAST(fr.ceil_height,bk.ceil_height)<=GREATEST(fr.floor_height,bk.floor_height)
  ),
  intersections AS (
    SELECT r.map_id,r.projectile_id,r.ray_index,t.thing_id,
           r.ox,r.oy,t.x,t.y,r.spray_range,r.spray_dice,r.spray_random_base,
           r.dmg_dice_count,
           g.along-SQRT(GREATEST(0.0,t.radius*t.radius-g.perp2)) AS distance
    FROM rays r CROSS JOIN targets t
    CROSS JOIN LATERAL (
      SELECT (t.x-r.ox)*COS(r.ray_angle)+(t.y-r.oy)*SIN(r.ray_angle) AS along,
             POWER(-(t.x-r.ox)*SIN(r.ray_angle)+(t.y-r.oy)*COS(r.ray_angle),2) AS perp2
    ) g
    WHERE t.thing_id<>r.owner_thing_id AND g.along>0
      AND g.along<=r.spray_range+t.radius
      AND g.perp2<=t.radius*t.radius
  ),
  visible AS (
    SELECT i.* FROM intersections i
    WHERE i.distance>0 AND i.distance<=i.spray_range
      AND NOT EXISTS (
        SELECT 1 FROM blocking b CROSS JOIN LATERAL (
          SELECT (i.ox-b.x1)*(b.y2-b.y1)-(i.oy-b.y1)*(b.x2-b.x1) AS d1,
                 (i.x-b.x1)*(b.y2-b.y1)-(i.y-b.y1)*(b.x2-b.x1) AS d2,
                 (b.x1-i.ox)*(i.y-i.oy)-(b.y1-i.oy)*(i.x-i.ox) AS d3,
                 (b.x2-i.ox)*(i.y-i.oy)-(b.y2-i.oy)*(i.x-i.ox) AS d4
        ) q WHERE q.d1*q.d2<0 AND q.d3*q.d4<0
      )
  ),
  ranked AS (
    SELECT i.*,ROW_NUMBER() OVER (PARTITION BY projectile_id,ray_index
                                 ORDER BY distance, thing_id) AS rn
    FROM visible i
  ),
  hits AS (SELECT * FROM ranked WHERE rn=1)
  INSERT INTO projectile_damage
    (map_id,projectile_id,damage_kind,hit_index,thing_id,damage)
  SELECT h.map_id,h.projectile_id,'bfg_spray',h.ray_index,h.thing_id,
         -- A_BFGSpray rolls 15d8 per ray
         (SELECT SUM(doom_prandom(h.thing_id, h.projectile_id*64 + h.ray_index,
                                 h.spray_random_base + g.i) % h.dmg_dice_count + 1)
         FROM generate_series(0, h.spray_dice - 1) g(i))
  FROM hits h
  ON CONFLICT (map_id, projectile_id, damage_kind, hit_index) DO NOTHING;
  -- apply accumulated actor damage
  UPDATE thing_health h
  SET health=h.health-d.damage,alive=h.health-d.damage>0
  FROM (
    SELECT map_id,thing_id,SUM(damage)::int AS damage
    FROM projectile_damage WHERE map_id=a_map_id GROUP BY map_id,thing_id
  ) d
  WHERE h.map_id=d.map_id AND h.thing_id=d.thing_id;
  --P_DamageMobj's thrust
  UPDATE things t
  SET mom_x = (t.mom_x + s.ddx)::real,
      mom_y = (t.mom_y + s.ddy)::real
  FROM (
    SELECT q.thing_id,
           SUM(ROUND(q.push * q.dx / q.dist * 65536)) / 65536.0 AS ddx,
           SUM(ROUND(q.push * q.dy / q.dist * 65536)) / 65536.0 AS ddy
    FROM (
      SELECT pd.thing_id,
             pd.damage * 12.5 / GREATEST(1, cd.mass) AS push,
             (v.x - src.x)::float8 AS dx, (v.y - src.y)::float8 AS dy,
             NULLIF(SQRT(POWER(v.x-src.x,2)+POWER(v.y-src.y,2)),0)::float8 AS dist
      FROM projectile_damage pd
      JOIN things v ON v.map_id=pd.map_id AND v.id=pd.thing_id
      JOIN thing_combat_defs cd ON cd.thing_type=v.type
      CROSS JOIN LATERAL (
        SELECT CASE WHEN pd.damage_kind='bfg_spray' THEN pl.x ELSE i.x END AS x,
               CASE WHEN pd.damage_kind='bfg_spray' THEN pl.y ELSE i.y END AS y
        FROM things pl
        LEFT JOIN projectile_impacts i ON i.map_id=pd.map_id
         AND i.projectile_id=pd.projectile_id
        WHERE pl.map_id=pd.map_id AND pl.id=a_player_thing_id::int
      ) src
      WHERE pd.map_id=a_map_id AND pd.damage>0
        AND NOT EXISTS (SELECT 1 FROM player_state ps
                        WHERE ps.map_id=pd.map_id AND ps.player_thing_id=pd.thing_id)
    ) q
    WHERE q.dist IS NOT NULL
    GROUP BY q.thing_id
  ) s
  WHERE t.map_id=a_map_id AND t.id=s.thing_id;
  -- a hurt monster turns on whatever hit it
  UPDATE monster_ai ai
  SET target_thing_id = src.owner_thing_id,
      state = CASE WHEN ai.state = 'stand' THEN 'see'::actor_state ELSE ai.state END,
      seq_index = CASE WHEN ai.state = 'stand' THEN 0 ELSE ai.seq_index END,
      state_tics = CASE WHEN ai.state = 'stand' THEN 0 ELSE ai.state_tics END
  FROM (
    SELECT d.thing_id, ARG_MAX(i.owner_thing_id, i.projectile_id) AS owner_thing_id
    FROM projectile_damage d
    JOIN projectile_impacts i ON i.map_id = d.map_id
     AND i.projectile_id = d.projectile_id
    WHERE d.map_id = a_map_id
      AND (i.projectile_type IN ('imp_fireball','baron_fireball','caco_fireball')
           OR EXISTS (SELECT 1 FROM monster_ai mo WHERE mo.map_id=i.map_id AND mo.thing_id=i.owner_thing_id))
      AND i.owner_thing_id IS NOT NULL
    GROUP BY d.thing_id
  ) src
  WHERE ai.map_id = a_map_id AND ai.thing_id = src.thing_id
    AND ai.thing_id <> src.owner_thing_id
    AND ai.target_thing_id IS DISTINCT FROM src.owner_thing_id;
  -- put surviving hit actors into pain
  WITH pain_rolls AS (
    SELECT d.map_id,d.thing_id,
           BOOL_OR(doom_prandom(d.thing_id, d.projectile_id, 11)
                     < cd.pain_chance) AS flinches,
           t.type
    FROM projectile_damage d
    JOIN things t ON t.map_id=d.map_id AND t.id=d.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type
    WHERE d.map_id=a_map_id AND d.damage>0
    GROUP BY d.map_id,d.thing_id,t.type
  )
  UPDATE monster_ai ai
  SET state=CASE WHEN hit.flinches THEN 'pain' ELSE 'see' END,
      seq_index=CASE WHEN hit.flinches THEN 0 ELSE ai.seq_index END,
      state_tics=CASE WHEN hit.flinches THEN COALESCE(fp.tics,ai.state_tics)
                      ELSE COALESCE(fs.tics,ai.state_tics) END
  FROM pain_rolls hit
  JOIN thing_health h ON h.map_id=hit.map_id AND h.thing_id=hit.thing_id
  LEFT JOIN thing_ai_frames fp ON fp.thing_type=hit.type
    AND fp.state='pain' AND fp.seq_index=0
  LEFT JOIN thing_ai_frames fs ON fs.thing_type=hit.type
    AND fs.state='see' AND fs.seq_index=0
  WHERE ai.map_id=hit.map_id AND ai.thing_id=hit.thing_id
    AND h.alive AND ai.state NOT IN ('die','dead')
    AND NOT EXISTS (SELECT 1 FROM thing_combat_defs bd
                    WHERE bd.thing_type = hit.type AND bd.explodes)
    AND (hit.flinches OR ai.state='stand');
  -- visible BFG spray explosions
  INSERT INTO world_effects
    (map_id,effect_id,effect_type,x,y,z,sector_id,age)
  SELECT d.map_id,v_effect_id_base::bigint+d.projectile_id*100+d.hit_index,
         'bfg_spray',t.x,t.y,sec.floor_height+rt.thing_height/4.0,
         COALESCE(ai.sector_id,rt.sector_id),0
  FROM projectile_damage d
  JOIN things t ON t.map_id=d.map_id AND t.id=d.thing_id
  JOIN render_things rt ON rt.map_id=t.map_id AND rt.thing_id=t.id
  LEFT JOIN monster_ai ai ON ai.map_id=t.map_id AND ai.thing_id=t.id
  JOIN sectors sec ON sec.map_id=t.map_id
    AND sec.id=COALESCE(ai.sector_id,rt.sector_id)
  WHERE d.map_id=a_map_id AND d.damage_kind='bfg_spray'
  ON CONFLICT (map_id,effect_id) DO NOTHING;
  -- projectile damage landing on players
  WITH
  params AS (SELECT a_map_id::int AS map_id),
  landed AS (
    SELECT pd.thing_id AS victim_id, pd.damage,
           CASE WHEN pd.damage_kind='bfg_spray' THEN ot.x ELSE i.x END::float8 AS ix,
           CASE WHEN pd.damage_kind='bfg_spray' THEN ot.y ELSE i.y END::float8 AS iy,
           COALESCE(k.player_thing_id, -1) AS source_id
    FROM projectile_damage pd
    JOIN params p ON pd.map_id=p.map_id
    JOIN player_state v ON v.map_id=pd.map_id AND v.player_thing_id=pd.thing_id
    JOIN monster_projectiles mp ON mp.map_id=pd.map_id AND mp.projectile_id=pd.projectile_id
    LEFT JOIN projectile_impacts i ON i.map_id=pd.map_id AND i.projectile_id=pd.projectile_id
    LEFT JOIN things ot ON ot.map_id=mp.map_id AND ot.id=mp.owner_thing_id
    LEFT JOIN player_state k ON k.map_id=mp.map_id AND k.player_thing_id=mp.owner_thing_id
    WHERE pd.damage>0
  ),
  scaled AS (
    SELECT l.victim_id,
           l.damage >> CASE WHEN g.skill=0 THEN 1 ELSE 0 END AS dmg,
           l.ix,l.iy,l.source_id,
           vt.x::float8 AS vx,vt.y::float8 AS vy
    FROM landed l
    JOIN player_state v ON v.map_id=a_map_id::int AND v.player_thing_id=l.victim_id
    JOIN game_tic_commands g ON g.map_id=v.map_id AND g.player_thing_id=v.player_thing_id
    JOIN things vt ON vt.map_id=v.map_id AND vt.id=v.player_thing_id
  ),
  totals AS (
    SELECT victim_id,
           SUM(dmg)::int AS dmg,
           SUM(FLOOR(dmg/3.0))::int AS green_saved,
           SUM(FLOOR(dmg/2.0))::int AS blue_saved,
           ARG_MAX(source_id, dmg::bigint*1000000 + source_id) AS source_id,
           -- Summed in 16.16 fixed point, as vanilla adds momentum: a float sum
           -- of several hits depends on the order the rows arrive in.
           SUM(ROUND(CASE WHEN r.dist>0 THEN dmg*0.125*(vx-ix)/r.dist ELSE 0.0 END * 65536.0)) / 65536.0 AS thrust_x,
           SUM(ROUND(CASE WHEN r.dist>0 THEN dmg*0.125*(vy-iy)/r.dist ELSE 0.0 END * 65536.0)) / 65536.0 AS thrust_y
    FROM scaled s
    CROSS JOIN LATERAL (SELECT SQRT(POWER(s.vx-s.ix,2)+POWER(s.vy-s.iy,2)) AS dist) r
    GROUP BY victim_id
  ),
  absorbed AS (
    SELECT t.*,
           (ps.god_mode OR ps.invuln_tics>0) AS blocked,
           CASE WHEN ps.god_mode OR ps.invuln_tics>0 THEN 0
                ELSE LEAST(ps.armor,CASE ps.armor_class
                       WHEN 2 THEN t.blue_saved WHEN 1 THEN t.green_saved
                       ELSE 0 END)
           END AS saved
    FROM totals t
    JOIN player_state ps ON ps.map_id=a_map_id::int AND ps.player_thing_id=t.victim_id
  ),
  applied AS (
    SELECT a.*,CASE WHEN a.blocked THEN 0 ELSE a.dmg-a.saved END AS took
    FROM absorbed a
  )
  UPDATE player_state ps
  SET armor=ps.armor-a.saved,
      armor_class=CASE WHEN ps.armor-a.saved<=0 THEN 0 ELSE ps.armor_class END,
      health=GREATEST(0,ps.health-a.took),
      damage_count=LEAST(100,ps.damage_count+a.took),
      alive=GREATEST(0,ps.health-a.took)>0,
      pain_face_tics=CASE WHEN a.took>0 THEN 12 ELSE ps.pain_face_tics END,
      killer_id=CASE WHEN ps.alive AND ps.health-a.took<=0 THEN a.source_id
                     ELSE ps.killer_id END,
      momentum_x=(ps.momentum_x+a.thrust_x)::real,
      momentum_y=(ps.momentum_y+a.thrust_y)::real
  FROM applied a
  WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a.victim_id;
  -- enter impact state
  UPDATE monster_projectiles mp
  SET x=i.x,y=i.y,z=i.z,state='explode',age=0,
      impact_player=i.target_player
  FROM projectile_impacts i
  WHERE mp.map_id=a_map_id AND i.map_id=mp.map_id
    AND i.projectile_id=mp.projectile_id AND mp.state='fly';
  -- advance unobstructed flights
  UPDATE monster_projectiles mp
  SET x=mp.x+mp.vx,y=mp.y+mp.vy,z=mp.z+mp.vz,
      age=mp.age+1,impact_player=FALSE
  WHERE mp.map_id=a_map_id AND mp.state='fly'
    AND NOT EXISTS (
      SELECT 1 FROM projectile_impacts i
      WHERE i.map_id=mp.map_id AND i.projectile_id=mp.projectile_id
    );
  -- advance explosion animations
  UPDATE monster_projectiles
  SET age=age+1,impact_player=FALSE
  WHERE map_id=a_map_id AND state='explode';
  -- retire completed missiles
  -- Retire a missile when its explosion animation has run out, or when one
  -- that never hit anything has been in the air too long.
  DELETE FROM monster_projectiles mp
  USING projectile_defs pd
  WHERE mp.map_id=a_map_id AND pd.projectile_type=mp.projectile_type
    AND ((mp.state='explode' AND mp.age>=pd.explode_tics)
      OR (mp.state='fly' AND mp.age>=pd.fly_timeout));
  RETURN 0;
END;;

-- ==== 25_cs_sound.sql ====
DROP FUNCTION IF EXISTS doom_cs_sound;;
CREATE FUNCTION doom_cs_sound(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_active_sound_odds DOUBLE;
  DECLARE v_sound_trim_age DOUBLE;
  DECLARE v_sound_trim_interval DOUBLE;
  DECLARE v_level_tic BIGINT;
  DECLARE v_t BIGINT;
  SET v_active_sound_odds = doom_const('ACTIVE_SOUND_ODDS');
  SET v_sound_trim_age = doom_const('SOUND_TRIM_AGE_TICS');
  SET v_sound_trim_interval = doom_const('SOUND_TRIM_INTERVAL_TICS');
  -- Emit one tic's sound events
  WITH
  pickup AS (
    SELECT map_id,player_thing_id,COUNT(*)>0 AS changed
    FROM pickup_touches GROUP BY map_id,player_thing_id
  ),
  weapon_grant AS (
    SELECT map_id,player_thing_id,COUNT(*)>0 AS granted
    FROM pickup_grants GROUP BY map_id,player_thing_id
  ),
  input AS (
    SELECT g.map_id,g.player_thing_id,ps.level_tics AS level_tic,
           COALESCE(p.changed,FALSE) AS pickup_changed,
           COALESCE(gr.granted,FALSE) AS weapon_granted,g.use_requested
    FROM game_tic_commands g
    JOIN player_state ps ON ps.map_id=g.map_id
      AND ps.player_thing_id=g.player_thing_id
    LEFT JOIN pickup p ON p.map_id=g.map_id AND p.player_thing_id=g.player_thing_id
    LEFT JOIN weapon_grant gr ON gr.map_id=g.map_id
      AND gr.player_thing_id=g.player_thing_id
    WHERE g.map_id=a_map_id::int AND g.player_thing_id=a_player_thing_id::int
  ),
  player_pos AS (
    SELECT i.*,t.x::real AS px,t.y::real AS py
    FROM input i JOIN things t ON t.map_id=i.map_id AND t.id=i.player_thing_id
  ),
  sound_pick AS (
    SELECT d.thing_type, d.cue, d.variant, d.sound_name, 1 AS n
    FROM thing_sound_defs d
  ),
  events AS (
    SELECT p.map_id,'player-fire:'||p.player_thing_id::text||':'||pw.shot_serial::text AS event_key,
           p.level_tic,
           CASE pw.current_weapon
             WHEN 1 THEN 'DSPUNCH' WHEN 2 THEN 'DSPISTOL'
             WHEN 3 THEN 'DSSHOTGN' WHEN 4 THEN 'DSPISTOL'
             WHEN 5 THEN 'DSRLAUNC' WHEN 6 THEN 'DSPLASMA'
             WHEN 7 THEN 'DSBFG' WHEN 8 THEN NULL
           END AS sound_name,p.player_thing_id AS source_thing_id,
           p.px AS source_x,p.py AS source_y
    FROM player_pos p JOIN player_weapons pw
      ON pw.map_id=p.map_id AND pw.player_thing_id=p.player_thing_id
    WHERE pw.fired_this_tick

    UNION ALL
    SELECT p.map_id,'pickup:'||p.player_thing_id::text||':'||p.level_tic::text,p.level_tic,
           CASE WHEN p.weapon_granted THEN 'DSWPNUP' ELSE 'DSITEMUP' END,
           p.player_thing_id,p.px,p.py
    FROM player_pos p WHERE p.pickup_changed

    UNION ALL
    SELECT p.map_id,'use:'||p.player_thing_id::text||':'||p.level_tic::text,p.level_tic,
           CASE
             WHEN COALESCE(r.locked,FALSE) THEN 'DSOOF'
             WHEN COALESCE(r.eligible,FALSE) AND r.special IN
                  (1,26,27,28,31,32,33,34,117,118) THEN 'DSDOROPN'
             WHEN COALESCE(r.eligible,FALSE) THEN 'DSSWTCHN'
             ELSE 'DSNOWAY'
           END,
           p.player_thing_id,p.px,p.py
    FROM player_pos p
    LEFT JOIN line_use_results r ON r.map_id=p.map_id
      AND r.player_thing_id=p.player_thing_id
    WHERE p.use_requested

    UNION ALL
    SELECT p.map_id,
           'monster-fire:'||ai.thing_id::text||':'||p.level_tic::text,
           p.level_tic,
           CASE
             WHEN d.attack_sound IS NOT NULL THEN d.attack_sound
             WHEN d.missile_type IS NOT NULL
                  AND NOT (d.melee_mult IS NOT NULL
                           AND sqrt(power(t.x-p.px,2)+power(t.y-p.py,2))<=64.0)
               THEN 'DSFIRSHT'
             ELSE 'DSCLAW'
           END,
           ai.thing_id,t.x::real,t.y::real
    FROM player_pos p JOIN monster_ai ai ON ai.map_id=p.map_id
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_combat_defs d ON d.thing_type=t.type
    WHERE ai.fired_this_tick

    UNION ALL
    SELECT p.map_id,'impact:'||i.projectile_id::text,p.level_tic,
           CASE i.projectile_type WHEN 'rocket' THEN 'DSRXPLOD'
                ELSE 'DSFIRXPL' END,
           i.owner_thing_id,i.x::real,i.y::real
    FROM player_pos p JOIN projectile_impacts i ON i.map_id=p.map_id

    UNION ALL
    SELECT p.map_id,'mon-sight:'||ai.thing_id::text,p.level_tic,sp.sound_name,
           ai.thing_id,t.x::real,t.y::real
    FROM player_pos p
    JOIN monster_ai ai ON ai.map_id=p.map_id AND ai.state <> 'stand'
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_health h ON h.map_id=ai.map_id AND h.thing_id=ai.thing_id
     AND h.alive
    JOIN sound_pick sp ON sp.thing_type=t.type AND sp.cue='sight'
     AND sp.variant = doom_prandom(ai.thing_id, 0, 60) % sp.n

    UNION ALL
    SELECT p.map_id,'mon-death:'||ai.thing_id::text,p.level_tic,sp.sound_name,
           ai.thing_id,t.x::real,t.y::real
    FROM player_pos p
    JOIN monster_ai ai ON ai.map_id=p.map_id
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_health h ON h.map_id=ai.map_id AND h.thing_id=ai.thing_id
     AND NOT h.alive
    -- A_XScream: a gibbed actor slops instead of crying out.
    JOIN thing_health hh ON hh.map_id=ai.map_id AND hh.thing_id=ai.thing_id
    JOIN thing_combat_defs cdx ON cdx.thing_type=t.type
    JOIN sound_pick sp ON sp.thing_type=t.type
     AND sp.cue = CASE WHEN hh.health < -hh.max_health
                        AND cdx.xdeath_frame IS NOT NULL
                       THEN 'xdeath'::sound_cue ELSE 'death'::sound_cue END
     AND sp.variant = doom_prandom(ai.thing_id, 0, 60) % sp.n

    UNION ALL
    -- Pain
    SELECT p.map_id,
           'mon-pain:'||ai.thing_id::text||':'||p.level_tic::text,p.level_tic,
           sp.sound_name,ai.thing_id,t.x::real,t.y::real
    FROM player_pos p
    JOIN monster_ai ai ON ai.map_id=p.map_id AND ai.state='pain'
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_ai_frames f ON f.thing_type=t.type AND f.state='pain'
     AND f.seq_index=ai.seq_index AND ai.state_tics=f.tics
    JOIN sound_pick sp ON sp.thing_type=t.type AND sp.cue='pain'
     AND sp.variant = doom_prandom(ai.thing_id, 0, 60) % sp.n
    UNION ALL
    -- Chase
    SELECT p.map_id,
           'mon-act:'||ai.thing_id::text||':'||p.level_tic::text,p.level_tic,
           sp.sound_name,ai.thing_id,t.x::real,t.y::real
    FROM player_pos p
    JOIN monster_ai ai ON ai.map_id=p.map_id AND ai.state='see'
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_health h ON h.map_id=ai.map_id AND h.thing_id=ai.thing_id
     AND h.alive
    JOIN sound_pick sp ON sp.thing_type=t.type AND sp.cue='active'
     AND sp.variant = doom_prandom(ai.thing_id, 0, 60) % sp.n
    WHERE (doom_prandom(ai.thing_id, p.level_tic, 61) * 256
           + doom_prandom(ai.thing_id, p.level_tic, 62)) % v_active_sound_odds = 0
    UNION ALL
    -- Player pain
    SELECT p.map_id,
           'plr-pain:'||p.player_thing_id::text||':'||(p.level_tic - (12 - ps.pain_face_tics))::text,
           p.level_tic,sp.sound_name,p.player_thing_id,p.px,p.py
    FROM player_pos p
    JOIN player_state ps ON ps.map_id=p.map_id
     AND ps.player_thing_id=p.player_thing_id
    JOIN sound_pick sp ON sp.thing_type=1 AND sp.cue='pain' AND sp.variant=0
    WHERE ps.alive AND ps.pain_face_tics > 0

    UNION ALL
    -- Player death
    SELECT p.map_id,'plr-death:'||p.player_thing_id::text,p.level_tic,sp.sound_name,
           p.player_thing_id,p.px,p.py
    FROM player_pos p
    JOIN player_state ps ON ps.map_id=p.map_id
     AND ps.player_thing_id=p.player_thing_id
    JOIN sound_pick sp ON sp.thing_type=1 AND sp.cue='death' AND sp.variant=0
    WHERE NOT ps.alive
  )
  INSERT INTO sound_events
    (map_id,event_key,level_tic,sound_name,source_thing_id,source_x,source_y)
  SELECT map_id,event_key,level_tic,sound_name,source_thing_id,source_x,source_y
  FROM events WHERE sound_name IS NOT NULL
  ON CONFLICT (map_id,event_key) DO NOTHING;
  -- doom_cs_projectiles has consumed monster firing flags before this runs.
  -- Clear them here so a held attack frame cannot emit the same sound on more
  -- than one physics tic before doom_cs_monsters advances again.
  UPDATE monster_ai SET fired_this_tick=FALSE
  WHERE map_id=a_map_id AND fired_this_tick;
  SET v_level_tic = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT ps.level_tics AS t FROM player_state ps
      WHERE ps.map_id = a_map_id AND ps.player_thing_id = a_player_thing_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_t;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_level_tic = v_t;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_level_tic >= 0 AND v_level_tic % v_sound_trim_interval = 0 THEN
    DELETE FROM sound_events e
      WHERE e.map_id = a_map_id
        AND e.level_tic < (SELECT COALESCE(MAX(ps.level_tics), 0) - v_sound_trim_age
                           FROM player_state ps
                           JOIN mp_players mp ON mp.map_id = ps.map_id
                            AND mp.player_thing_id = ps.player_thing_id
                           WHERE mp.map_id = a_map_id)
        AND e.event_key NOT LIKE 'mon-sight:%'
        AND e.event_key NOT LIKE 'mon-death:%'
        AND e.event_key NOT LIKE 'plr-death:%';
  END IF;
  RETURN 0;
END;;

-- ==== 25a_monster_views.sql ====

CREATE OR REPLACE VIEW doom_player_shadow AS
SELECT ps.map_id, ps.player_thing_id,
       (ps.invis_tics > 0) AS shadowed, ps.level_tics
FROM player_state ps;;


CREATE OR REPLACE VIEW doom_player_pos AS
SELECT t.map_id, t.id AS player_thing_id,
       t.x::double precision AS px, t.y::double precision AS py
FROM things t;;


CREATE OR REPLACE VIEW doom_monster_attackers AS
SELECT ai.map_id, ai.thing_id, t.type,
       t.x::double precision AS mx, t.y::double precision AS my,
       d.hitscan_pellets, d.melee_mult, d.melee_sides, d.explodes,
       d.blast_radius,
       d.hitscan_mult, d.hitscan_sides
FROM monster_ai ai
JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
JOIN thing_combat_defs d ON d.thing_type = t.type
WHERE ai.fired_this_tick;;

-- ==== 26_cs_monsters.sql ====
--   1 noise      the player fired, so sleeping monsters may hear it
--   2 stepping   a chaser is on a frame that moves it
--   4 floater    a cacodemon or lost soul is awake
--   8 chasing    something is in 'see'
--  16 attacking  something landed on an attack frame
--  32 barrel     one of those is a barrel going off
--  64 blast      a barrel or a rocket has a radius to push with
--
DROP FUNCTION IF EXISTS doom_cs_monster_plan;;
CREATE FUNCTION doom_cs_monster_plan(a_map_id INT) RETURNS BIGINT
BEGIN
  DECLARE v_flags BIGINT;
  DECLARE v_noise_due BOOLEAN;
  DECLARE v_stepping_due INT;
  DECLARE v_floater_due BOOLEAN;
  DECLARE v_chasing_due BOOLEAN;
  DECLARE v_attacking_due BOOLEAN;
  DECLARE v_barrel_due BOOLEAN;
  DECLARE v_rocket_due BOOLEAN;
  SET v_flags = 0::bigint;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT
        EXISTS (SELECT 1 FROM player_weapons pw
                WHERE pw.map_id=a_map_id AND pw.fired_this_tick) AS noise_due,
        EXISTS (
          SELECT 1 FROM monster_ai ai
          JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
          JOIN thing_ai_frames f ON f.thing_type=t.type AND f.state='see'
            AND f.seq_index=ai.seq_index
          JOIN thing_combat_defs cd ON cd.thing_type=t.type AND NOT cd.explodes
          WHERE ai.map_id=a_map_id AND ai.state='see'
            AND (ai.state_tics=f.tics OR ai.state_tics=FLOOR(f.tics/2.0)::int)
        ) AS stepping_due,
        EXISTS (
          SELECT 1 FROM monster_ai ai
          JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
          JOIN thing_combat_defs d ON d.thing_type=t.type
          WHERE ai.map_id=a_map_id AND d.floats AND ai.state IN ('see','missile')
        ) AS floater_due,
        EXISTS (SELECT 1 FROM monster_ai
                WHERE map_id=a_map_id AND state='see') AS chasing_due,
        EXISTS (SELECT 1 FROM monster_ai
                WHERE map_id=a_map_id AND fired_this_tick) AS attacking_due,
        EXISTS (
          SELECT 1 FROM monster_ai ai
          JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
          JOIN thing_combat_defs cd ON cd.thing_type=t.type AND cd.explodes
          WHERE ai.map_id=a_map_id AND ai.fired_this_tick
        ) AS barrel_due,
        EXISTS (SELECT 1 FROM projectile_impacts
                WHERE map_id=a_map_id AND projectile_type='rocket') AS rocket_due;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_noise_due, v_stepping_due, v_floater_due, v_chasing_due, v_attacking_due, v_barrel_due, v_rocket_due;
      IF b1_done THEN LEAVE b1_each; END IF;
      IF v_noise_due THEN
        SET v_flags = v_flags + 1;
      END IF;
      IF v_stepping_due THEN
        SET v_flags = v_flags + 2;
      END IF;
      IF v_floater_due THEN
        SET v_flags = v_flags + 4;
      END IF;
      IF v_chasing_due THEN
        SET v_flags = v_flags + 8;
      END IF;
      IF v_attacking_due THEN
        SET v_flags = v_flags + 16;
      END IF;
      IF v_barrel_due THEN
        SET v_flags = v_flags + 32;
      END IF;
      IF v_barrel_due OR v_rocket_due THEN
        SET v_flags = v_flags + 64;
      END IF;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_flags;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_retarget;;
CREATE FUNCTION doom_cs_monster_retarget(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Targets. NULL means "the player" for a single-player world; in a
  -- deathmatch the tic passes a different live player each time, so a monster
  -- that is up and has no target takes the one it is handed and keeps it until
  -- that player dies or leaves.
  UPDATE monster_ai ai
  SET target_thing_id = NULL
  WHERE ai.map_id = a_map_id AND ai.target_thing_id IS NOT NULL
    AND (EXISTS (
      SELECT 1 FROM player_state ps
      WHERE ps.map_id = ai.map_id AND ps.player_thing_id = ai.target_thing_id
        AND (NOT ps.alive OR EXISTS (
          SELECT 1 FROM mp_players mp
          WHERE mp.map_id = ps.map_id AND mp.player_thing_id = ps.player_thing_id
            AND mp.role_name IS NULL)))
      OR EXISTS (
      SELECT 1 FROM thing_health th
      WHERE th.map_id = ai.map_id AND th.thing_id = ai.target_thing_id AND NOT th.alive));
  UPDATE monster_ai
  SET target_thing_id = a_player_thing_id
  WHERE map_id = a_map_id AND target_thing_id IS NULL AND state IN ('see', 'missile', 'pain');
  RETURN 0;
END;;

-- noise alert
-- Firing wakes everything that can hear it. Doom floods the sound out from
-- the shooter's sector through P_RecursiveSound: it crosses a two-sided line
-- whose opening is not shut, and an ML_SOUNDBLOCK line absorbs one level, so
-- sound passes at most one of them before it stops.
DROP FUNCTION IF EXISTS doom_cs_monster_noise;;
CREATE FUNCTION doom_cs_monster_noise(a_map_id INT) RETURNS INT
BEGIN
  WITH RECURSIVE
  noise_origin AS (
    SELECT ps.sector_id
    FROM player_state ps
    JOIN player_weapons pw ON pw.map_id = ps.map_id
     AND pw.player_thing_id = ps.player_thing_id
    WHERE ps.map_id = a_map_id::int
      AND ps.alive AND pw.fired_this_tick AND ps.sector_id IS NOT NULL
  ),
  sound_edges AS (
    SELECT ld.fsec AS a, ld.bsec AS b,
           CASE WHEN (ld.flags & 64) <> 0 THEN 1 ELSE 0 END AS block
    FROM linedef_geom ld
    JOIN sectors sf ON sf.map_id = ld.map_id AND sf.id = ld.fsec
    JOIN sectors sb ON sb.map_id = ld.map_id AND sb.id = ld.bsec
    WHERE ld.map_id = a_map_id::int
      AND LEAST(sf.ceil_height, sb.ceil_height)
        - GREATEST(sf.floor_height, sb.floor_height) > 0
    UNION ALL
    SELECT ld.bsec, ld.fsec,
           CASE WHEN (ld.flags & 64) <> 0 THEN 1 ELSE 0 END
    FROM linedef_geom ld
    JOIN sectors sf ON sf.map_id = ld.map_id AND sf.id = ld.fsec
    JOIN sectors sb ON sb.map_id = ld.map_id AND sb.id = ld.bsec
    WHERE ld.map_id = a_map_id::int
      AND LEAST(sf.ceil_height, sb.ceil_height)
        - GREATEST(sf.floor_height, sb.floor_height) > 0
  ),
  flood AS (
    SELECT sector_id, 0 AS block FROM noise_origin
    UNION
    SELECT e.b, f.block + e.block
    FROM flood f JOIN sound_edges e ON e.a = f.sector_id
    WHERE f.block + e.block <= 1
  )
  UPDATE monster_ai ai
  SET state = 'see', seq_index = 0, state_tics = 0
  FROM things t, render_things rt, flood fl
  WHERE ai.map_id = a_map_id::int
    AND t.map_id = ai.map_id AND t.id = ai.thing_id
    AND rt.map_id = ai.map_id AND rt.thing_id = ai.thing_id
    AND ai.state = 'stand'
    AND NOT EXISTS (SELECT 1 FROM thing_combat_defs cd
                    WHERE cd.thing_type = t.type AND cd.explodes)
    AND COALESCE(ai.sector_id, rt.sector_id) = fl.sector_id;
  RETURN 0;
END;;

-- Activation (line of sight and range) and one step of every monster's state
-- and frame animation. Unconditional: this is what decides the rest of the
-- plan, so it runs before doom_cs_monster_plan is asked for the other bits.
DROP FUNCTION IF EXISTS doom_cs_monster_think;;
CREATE FUNCTION doom_cs_monster_think(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_attack_range_default DOUBLE;
  DECLARE v_sight_range DOUBLE;
  SET v_attack_range_default = doom_const('DEFAULT_ATTACK_RANGE');
  SET v_sight_range = doom_const('SIGHT_RANGE');
  -- activation + state/frame advancement
  WITH RECURSIVE
  params AS (SELECT a_map_id::int AS map_id, a_player_thing_id::int AS player_thing_id),
  player_pos AS (
    SELECT pp.px, pp.py
    FROM doom_player_pos pp JOIN params p ON pp.map_id = p.map_id
     AND pp.player_thing_id = p.player_thing_id
  ),
  -- MF_SHADOW on the target, which for the player means the blur sphere.
  shadow AS (
    SELECT sh.shadowed, sh.level_tics
    FROM doom_player_shadow sh JOIN params p ON sh.map_id = p.map_id
     AND sh.player_thing_id = p.player_thing_id
  ),
  blocking_walls AS (
    SELECT ld.x1::double precision AS x1, ld.y1::double precision AS y1,
           ld.x2::double precision AS x2, ld.y2::double precision AS y2
    FROM linedef_geom ld
    JOIN params p ON ld.map_id = p.map_id
    LEFT JOIN sectors fr ON fr.map_id = ld.map_id AND fr.id = ld.fsec
    LEFT JOIN sectors bk ON bk.map_id = ld.map_id AND bk.id = ld.bsec
    -- Sight is blocked by one-sided walls and by live two-sided portals with no
    -- vertical opening.
    WHERE ld.left_sd_id = -1 OR ld.right_sd_id = -1
       OR fr.id IS NULL OR bk.id IS NULL
       OR LEAST(fr.ceil_height, bk.ceil_height)
            <= GREATEST(fr.floor_height, bk.floor_height)
  ),
  monsters AS (
    SELECT ai.map_id, ai.thing_id, ai.state, ai.state_tics, ai.seq_index,
           ai.attack_cooldown, t.type, t.x::double precision AS mx,
           t.y::double precision AS my, t.angle::double precision AS facing,
           h.alive, h.health, h.max_health, cd.xdeath_frame,
           COALESCE(cd.explodes, FALSE) AS explodes,
           COALESCE(cd.attack_range, v_attack_range_default)::double precision AS attack_range
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
    JOIN thing_health h ON h.map_id = ai.map_id AND h.thing_id = ai.thing_id
    LEFT JOIN thing_combat_defs cd ON cd.thing_type = t.type
    WHERE h.alive OR ai.state <> 'dead'
  ),
  mon_target AS (
    SELECT ai.thing_id,
           COALESCE(tt.x::double precision, pp.px) AS tx,
           COALESCE(tt.y::double precision, pp.py) AS ty
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    CROSS JOIN player_pos pp
    LEFT JOIN (SELECT map_id, thing_id FROM thing_health WHERE alive
               UNION ALL SELECT map_id, player_thing_id FROM player_state WHERE alive) th
      ON th.map_id = ai.map_id AND th.thing_id = ai.target_thing_id
    LEFT JOIN things tt ON tt.map_id = ai.map_id AND tt.id = th.thing_id
  ),
  near_monsters AS (
    SELECT m.thing_id, m.mx, m.my, m.facing, mt.tx, mt.ty
    FROM monsters m
    JOIN mon_target mt ON mt.thing_id = m.thing_id
    WHERE power(m.mx - mt.tx, 2) + power(m.my - mt.ty, 2) <= v_sight_range * v_sight_range
  ),
  near_los_walls AS (
    SELECT nm.thing_id,
           BOOL_OR((sides.d1 * sides.d2 < 0) AND (sides.d3 * sides.d4 < 0)) AS blocked
    FROM near_monsters nm
    JOIN blocking_walls b
      ON LEAST(b.x1, b.x2) <= GREATEST(nm.mx, nm.tx)
     AND GREATEST(b.x1, b.x2) >= LEAST(nm.mx, nm.tx)
     AND LEAST(b.y1, b.y2) <= GREATEST(nm.my, nm.ty)
     AND GREATEST(b.y1, b.y2) >= LEAST(nm.my, nm.ty)
    CROSS JOIN LATERAL (
      SELECT
        (nm.mx - b.x1) * (b.y2 - b.y1) - (nm.my - b.y1) * (b.x2 - b.x1) AS d1,
        (nm.tx - b.x1) * (b.y2 - b.y1) - (nm.ty - b.y1) * (b.x2 - b.x1) AS d2,
        (b.x1 - nm.mx) * (nm.ty - nm.my) - (b.y1 - nm.my) * (nm.tx - nm.mx) AS d3,
        (b.x2 - nm.mx) * (nm.ty - nm.my) - (b.y2 - nm.my) * (nm.tx - nm.mx) AS d4
    ) sides
    GROUP BY nm.thing_id
  ),
  near_los AS (
    SELECT nm.thing_id,
      NOT COALESCE(w.blocked, FALSE) AS visible,
      (COS(RADIANS(nm.facing)) * (nm.tx - nm.mx)
       + SIN(RADIANS(nm.facing)) * (nm.ty - nm.my)) > 0 AS in_view_cone
    FROM near_monsters nm
    LEFT JOIN near_los_walls w ON w.thing_id = nm.thing_id
  ),
  los AS (
    SELECT m.thing_id,
      COALESCE(nl.visible, FALSE) AS visible,
      COALESCE(nl.in_view_cone, FALSE) AS in_view_cone,
      sqrt(power(m.mx - mt.tx, 2) + power(m.my - mt.ty, 2)) AS dist
    FROM monsters m
    JOIN mon_target mt ON mt.thing_id = m.thing_id
    LEFT JOIN near_los nl ON nl.thing_id = m.thing_id
  ),
  max_seq AS (
    SELECT thing_type, state, MAX(seq_index) AS max_idx
    FROM thing_ai_frames GROUP BY thing_type, state
  ),
  decision AS (
    SELECT
      m.map_id, m.thing_id, m.state, m.state_tics, m.seq_index,
      m.attack_cooldown, m.type, m.alive, m.health, m.max_health,
      m.xdeath_frame, m.explodes, l.visible, l.in_view_cone, l.dist,
      m.attack_range,
      ms.max_idx AS see_max, mm.max_idx AS missile_max,
      mp.max_idx AS pain_max, md.max_idx AS die_max,
      mx.max_idx AS xdeath_max
    FROM monsters m
    JOIN los l ON l.thing_id = m.thing_id
    LEFT JOIN max_seq ms ON ms.thing_type = m.type AND ms.state = 'see'
    LEFT JOIN max_seq mm ON mm.thing_type = m.type AND mm.state = 'missile'
    LEFT JOIN max_seq mp ON mp.thing_type = m.type AND mp.state = 'pain'
    LEFT JOIN max_seq md ON md.thing_type = m.type AND md.state = 'die'
    LEFT JOIN max_seq mx ON mx.thing_type = m.type AND mx.state = 'xdeath'
  ),
  transitions AS (
    SELECT d.*,
      d.state_tics <= 1
        OR (NOT d.alive AND d.state NOT IN ('die', 'dead', 'xdeath'))
        AS advances,
      CASE
        -- P_KillMobj: `health < -spawnhealth && xdeathstate` takes the extreme
        -- death rather than the ordinary one
        WHEN NOT d.alive AND d.state NOT IN ('die', 'dead', 'xdeath') THEN
          CASE WHEN d.health < -d.max_health AND d.xdeath_frame IS NOT NULL
               THEN 'xdeath'::actor_state ELSE 'die'::actor_state END
        WHEN d.state = 'die' THEN
          CASE WHEN d.state_tics > 1 THEN 'die'::actor_state
               WHEN d.seq_index >= d.die_max THEN 'dead'::actor_state
               ELSE 'die'::actor_state END
        WHEN d.state = 'xdeath' THEN
          CASE WHEN d.state_tics > 1 THEN 'xdeath'::actor_state
               WHEN d.seq_index >= d.xdeath_max THEN 'dead'::actor_state
               ELSE 'xdeath'::actor_state END
        -- Barrels use this same compact actor state table only for their death
        -- animation. While alive they never wake, chase, or attack.
        WHEN d.explodes THEN 'stand'::actor_state
        WHEN d.state = 'stand' THEN
          CASE WHEN d.visible AND d.in_view_cone AND d.dist <= v_sight_range
               THEN 'see'::actor_state ELSE 'stand'::actor_state END
        WHEN d.state_tics > 1 THEN d.state
        WHEN d.state = 'see' THEN
          CASE WHEN d.visible AND d.dist <= d.attack_range
                    AND d.attack_cooldown <= 0
               THEN 'missile'::actor_state ELSE 'see'::actor_state END
        WHEN d.state = 'missile' THEN
          CASE WHEN d.seq_index >= d.missile_max THEN 'see'::actor_state ELSE 'missile'::actor_state END
        WHEN d.state = 'pain' THEN
          CASE WHEN d.seq_index >= d.pain_max THEN 'see'::actor_state ELSE 'pain'::actor_state END
        ELSE d.state
      END AS next_state,
      CASE
        WHEN NOT d.alive AND d.state NOT IN ('die', 'dead', 'xdeath') THEN 0
        WHEN d.state = 'die' THEN
          CASE WHEN d.state_tics > 1 THEN d.seq_index
               WHEN d.seq_index >= d.die_max THEN 0
               ELSE d.seq_index + 1 END
        WHEN d.state = 'xdeath' THEN
          CASE WHEN d.state_tics > 1 THEN d.seq_index
               WHEN d.seq_index >= d.xdeath_max THEN 0
               ELSE d.seq_index + 1 END
        WHEN d.explodes THEN 0
        WHEN d.state = 'stand' THEN 0
        WHEN d.state_tics > 1 THEN d.seq_index
        WHEN d.state = 'see' THEN
          CASE WHEN d.visible AND d.dist <= d.attack_range
                    AND d.attack_cooldown <= 0
               THEN 0
               WHEN d.seq_index >= d.see_max THEN 0
               ELSE d.seq_index + 1 END
        WHEN d.state = 'missile' THEN
          CASE WHEN d.seq_index >= d.missile_max THEN 0 ELSE d.seq_index + 1 END
        WHEN d.state = 'pain' THEN
          CASE WHEN d.seq_index >= d.pain_max THEN 0 ELSE d.seq_index + 1 END
        ELSE d.seq_index
      END AS next_seq
    FROM decision d
  ),
  next_values AS (
    SELECT t.map_id, t.thing_id, t.state AS old_state, t.next_state,
           t.next_seq, t.type, t.advances,
           CASE
             WHEN NOT t.advances THEN t.state_tics - 1
             WHEN t.next_state = 'stand' THEN -1
             -- G_InitNew's fast-monster loop halves the tics of every state
             -- from S_SARG_RUN1 to S_SARG_PAIN2 on Nightmare
             WHEN cd.fast_on_nightmare
                  AND t.next_state IN ('see','missile','pain')
                  AND (SELECT g.skill FROM game_tic_commands g
                       WHERE g.map_id=a_map_id::int
                         AND g.player_thing_id=a_player_thing_id::int)=4
               THEN GREATEST(1, COALESCE(f.tics, -1) >> 1)
             ELSE COALESCE(f.tics, -1)
           END AS next_tics,
           CASE WHEN t.state = 'missile' AND t.next_state = 'see' THEN 30
                ELSE GREATEST(0, t.attack_cooldown - 1) END AS next_cooldown,
           COALESCE(f.is_attack_frame, FALSE) AS lands_on_attack_frame
    FROM transitions t
    JOIN thing_combat_defs cd ON cd.thing_type = t.type
    LEFT JOIN thing_ai_frames f
      ON f.thing_type = t.type AND f.state = t.next_state
     AND f.seq_index = t.next_seq
  )
  UPDATE monster_ai ai
  SET state = n.next_state, state_tics = n.next_tics,
      seq_index = n.next_seq, attack_cooldown = n.next_cooldown,
      fired_this_tick = n.advances AND n.lands_on_attack_frame
  FROM next_values n
  WHERE ai.map_id = n.map_id AND ai.thing_id = n.thing_id
    AND (ai.state <> n.next_state
      OR ai.state_tics <> n.next_tics
      OR ai.seq_index <> n.next_seq
      OR ai.attack_cooldown <> n.next_cooldown
      OR ai.fired_this_tick <> (n.advances AND n.lands_on_attack_frame));
  RETURN 0;
END;;

-- Stage this tic's chase moves into monster_steps. Returns whether any
-- survived the wall and thing checks, which is the gate on doom_cs_monster_move.
DROP FUNCTION IF EXISTS doom_cs_monster_step;;
CREATE FUNCTION doom_cs_monster_step(a_map_id INT, a_player_thing_id INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_blockmap_cell DOUBLE;
  DECLARE v_min_walk_opening DOUBLE;
  DECLARE v_max_step DOUBLE;
  DECLARE v_player_radius DOUBLE;
  DECLARE v_monster_step DOUBLE;
  DECLARE v_chase_deadband DOUBLE;
  DECLARE v_chase_swap_chance DOUBLE;
  DECLARE v_chase_movecount_mask DOUBLE;
  DECLARE v_stepped BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_blockmap_cell = doom_const('BLOCKMAP_CELL');
  SET v_min_walk_opening = doom_const('MIN_WALK_OPENING');
  SET v_max_step = doom_const('MAXSTEP');
  SET v_player_radius = doom_const('PLAYER_RADIUS');
  SET v_monster_step = doom_const('MONSTER_STEP');
  SET v_chase_deadband = doom_const('CHASE_AXIS_DEADBAND');
  SET v_chase_swap_chance = doom_const('CHASE_SWAP_CHANCE');
  SET v_chase_movecount_mask = doom_const('CHASE_MOVECOUNT_MASK');
  WITH
  params AS (SELECT a_map_id::int AS map_id, a_player_thing_id::int AS player_thing_id),
  player_pos AS (
    SELECT pp.px, pp.py
    FROM doom_player_pos pp JOIN params p ON pp.map_id = p.map_id
     AND pp.player_thing_id = p.player_thing_id
  ),
  -- MF_SHADOW on the target, which for the player means the blur sphere.
  shadow AS (
    SELECT sh.shadowed, sh.level_tics
    FROM doom_player_shadow sh JOIN params p ON sh.map_id = p.map_id
     AND sh.player_thing_id = p.player_thing_id
  ),
  chasers AS (
    SELECT ai.thing_id, t.x::double precision AS mx, t.y::double precision AS my,
           d.radius::double precision AS radius,
           d.floats, ai.movedir, ai.movecount
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
    JOIN thing_combat_defs d ON d.thing_type = t.type
    JOIN thing_ai_frames f ON f.thing_type=t.type AND f.state='see'
      AND f.seq_index=ai.seq_index
    WHERE ai.state='see' AND NOT d.explodes
      AND (ai.state_tics=f.tics
        OR ai.state_tics=FLOOR(f.tics/2.0)::int)
  ),
  mon_target AS (
    SELECT ai.thing_id,
           COALESCE(tt.x::double precision, pp.px) AS tx,
           COALESCE(tt.y::double precision, pp.py) AS ty,
           ai.target_thing_id
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    CROSS JOIN player_pos pp
    LEFT JOIN (SELECT map_id, thing_id FROM thing_health WHERE alive
               UNION ALL SELECT map_id, player_thing_id FROM player_state WHERE alive) th
      ON th.map_id = ai.map_id AND th.thing_id = ai.target_thing_id
    LEFT JOIN things tt ON tt.map_id = ai.map_id AND tt.id = th.thing_id
  ),
  walk AS (
    SELECT c.thing_id, c.mx, c.my, c.radius, c.floats, c.movedir, c.movecount,
           old.opposite AS turnaround,
           CASE WHEN mt.tx - c.mx >  v_chase_deadband THEN 0
                WHEN mt.tx - c.mx < -v_chase_deadband THEN 4 END AS dx_dir,
           CASE WHEN mt.ty - c.my < -v_chase_deadband THEN 6
                WHEN mt.ty - c.my >  v_chase_deadband THEN 2 END AS dy_dir,
           diag.dir AS diag,
           (doom_prandom(c.thing_id, sh.level_tics, 50) > v_chase_swap_chance
            OR ABS(mt.ty - c.my) > ABS(mt.tx - c.mx)) AS swap,
           (doom_prandom(c.thing_id, sh.level_tics, 51) & 1) = 1 AS sweep_up,
           (doom_prandom(c.thing_id, sh.level_tics, 52) & v_chase_movecount_mask::int)
             AS fresh_movecount,
           (c.movecount > 0 AND c.movedir IS NOT NULL) AS keep_going
    FROM chasers c
    JOIN mon_target mt ON mt.thing_id = c.thing_id
    CROSS JOIN shadow sh
    LEFT JOIN chase_dir_defs old ON old.dir = c.movedir
    JOIN chase_dir_defs diag ON diag.is_diagonal
     AND (diag.dx > 0) = (mt.tx - c.mx > 0)
     AND (diag.dy > 0) = (mt.ty - c.my >= 0)
  ),
  -- P_NewChaseDir tries directions in a fixed order and takes the first that works.
  ranked AS (
    SELECT w.thing_id, d.dir,
      LEAST(
        CASE WHEN w.keep_going AND d.dir = w.movedir THEN -1 ELSE 99 END,
        CASE WHEN w.dx_dir IS NOT NULL AND w.dy_dir IS NOT NULL AND d.dir = w.diag
              AND w.diag IS DISTINCT FROM w.turnaround THEN 0 ELSE 99 END,
        CASE WHEN d.dir = (CASE WHEN w.swap THEN w.dy_dir ELSE w.dx_dir END)
              AND d.dir IS DISTINCT FROM w.turnaround THEN 1 ELSE 99 END,
        CASE WHEN d.dir = (CASE WHEN w.swap THEN w.dx_dir ELSE w.dy_dir END)
              AND d.dir IS DISTINCT FROM w.turnaround THEN 2 ELSE 99 END,
        CASE WHEN d.dir = w.movedir THEN 3 ELSE 99 END,
        CASE WHEN d.dir IS DISTINCT FROM w.turnaround
             THEN 4 + CASE WHEN w.sweep_up THEN d.dir ELSE last.dir - d.dir END
             ELSE 99 END,
        CASE WHEN d.dir = w.turnaround THEN 12 ELSE 99 END
      ) AS pri
    FROM walk w CROSS JOIN chase_dir_defs d
    CROSS JOIN (SELECT MAX(dir) AS dir FROM chase_dir_defs) last
  ),
  desired AS (
    SELECT r.pri, w.thing_id, w.mx, w.my, w.radius, w.floats,
           w.mx + v_monster_step * d.dx AS nx, w.my + v_monster_step * d.dy AS ny,
           d.angle AS face_angle, d.dir,
           CASE WHEN r.pri = -1 THEN w.movecount - 1
                ELSE w.fresh_movecount END AS next_movecount
    FROM ranked r
    JOIN walk w ON w.thing_id = r.thing_id
    JOIN chase_dir_defs d ON d.dir = r.dir
    WHERE r.pri < 99
  ),
  blocking AS (
    SELECT ld.x1::double precision AS x1, ld.y1::double precision AS y1,
           ld.x2::double precision AS x2, ld.y2::double precision AS y2,
           -- blocked only by the 24-unit step rule: no wall for a floater
           (NOT (ld.left_sd_id = -1 OR ld.right_sd_id = -1 OR (ld.flags & 1) <> 0)
            AND NOT (sb.ceil_height IS NOT NULL AND sf.ceil_height IS NOT NULL
                     AND sb.floor_height IS NOT NULL AND sf.floor_height IS NOT NULL
                     AND (LEAST(sf.ceil_height, sb.ceil_height) - GREATEST(sf.floor_height, sb.floor_height)) < v_min_walk_opening))
             AS floor_step_only
    FROM linedef_geom ld
    JOIN params p ON ld.map_id = p.map_id
    LEFT JOIN sectors sf ON sf.map_id = ld.map_id AND sf.id = ld.fsec
    LEFT JOIN sectors sb ON sb.map_id = ld.map_id AND sb.id = ld.bsec
    WHERE ld.left_sd_id = -1 OR ld.right_sd_id = -1
       OR (ld.flags & 1) <> 0
       OR (sb.floor_height IS NOT NULL AND sf.floor_height IS NOT NULL
           AND ABS(sb.floor_height - sf.floor_height) > v_max_step)
       OR (sb.ceil_height IS NOT NULL AND sf.ceil_height IS NOT NULL
           AND sb.floor_height IS NOT NULL AND sf.floor_height IS NOT NULL
           AND (LEAST(sf.ceil_height, sb.ceil_height) - GREATEST(sf.floor_height, sb.floor_height)) < v_min_walk_opening)
  ),
  -- PIT_CheckThing for monsters. Everything solid blocks everything else, so a
  -- monster is stopped by another monster, by a barrel, by a solid decoration
  -- and by the player
  solid_things AS (
    SELECT tt.id AS thing_id, tt.x::double precision AS tx,
           tt.y::double precision AS ty, cd.radius::double precision AS radius,
           FLOOR(tt.x / v_blockmap_cell)::int AS cellx,
           FLOOR(tt.y / v_blockmap_cell)::int AS celly
    FROM things tt
    JOIN params p ON tt.map_id = p.map_id
    JOIN thing_combat_defs cd ON cd.thing_type = tt.type
    JOIN thing_health hh ON hh.map_id = tt.map_id AND hh.thing_id = tt.id
     AND hh.alive
    UNION ALL
    -- Solid decorations
    SELECT tt.id, tt.x::double precision, tt.y::double precision,
           bd.radius::double precision,
           FLOOR(tt.x / v_blockmap_cell)::int, FLOOR(tt.y / v_blockmap_cell)::int
    FROM things tt
    JOIN params p ON tt.map_id = p.map_id
    JOIN thing_blocking_defs bd ON bd.thing_type = tt.type
    JOIN game_tic_commands g ON g.map_id = tt.map_id
     AND g.player_thing_id = p.player_thing_id
    WHERE (tt.flags & g.skill_bit) <> 0 AND (tt.flags & 16) = 0
    UNION ALL
    -- Every live player
    SELECT ps.player_thing_id, t.x::double precision, t.y::double precision, v_player_radius,
           FLOOR(t.x / v_blockmap_cell)::int, FLOOR(t.y / v_blockmap_cell)::int
    FROM player_state ps
    JOIN params p ON ps.map_id = p.map_id
    JOIN things t ON t.map_id = ps.map_id AND t.id = ps.player_thing_id
    WHERE ps.alive
  ),
  candidates AS (
    SELECT d.pri, d.thing_id, d.nx AS cx, d.ny AS cy,
           d.mx AS old_x, d.my AS old_y, d.radius, d.face_angle, d.floats,
           d.dir, d.next_movecount
    FROM desired d
  ),
  thing_hit AS (
    SELECT c.pri, c.thing_id, TRUE AS hit
    FROM candidates c
    -- The 3x3 block of cells around the candidate: the largest radius sum in
    -- play is well under a cell, so a neighbour cell is as far as an overlap
    -- can reach.
    CROSS JOIN generate_series(-1,1) AS gx(d)   -- PostgreSQL's SELECT generate_series(..) (a set-returning function in a SELECT list), in FROM
    CROSS JOIN generate_series(-1,1) AS gy(d)
    JOIN solid_things st
      ON st.cellx = FLOOR(c.cx / v_blockmap_cell)::int + gx.d
     AND st.celly = FLOOR(c.cy / v_blockmap_cell)::int + gy.d
     AND st.thing_id <> c.thing_id
     AND ABS(st.tx - c.cx) < st.radius + c.radius
     AND ABS(st.ty - c.cy) < st.radius + c.radius
    GROUP BY c.pri, c.thing_id
  ),
  wall_block AS (
    -- Candidates against the walls whose box overlaps the step, aggregated to
    -- one verdict per candidate.
    SELECT c.pri, c.thing_id,
           BOOL_OR(dd.dist < c.radius OR xing.intersects) AS blocked
    FROM candidates c
    JOIN blocking b
      ON NOT (b.floor_step_only AND c.floats)
     AND LEAST(b.x1, b.x2) <= GREATEST(c.cx, c.old_x) + c.radius + 2.0
     AND GREATEST(b.x1, b.x2) >= LEAST(c.cx, c.old_x) - c.radius - 2.0
     AND LEAST(b.y1, b.y2) <= GREATEST(c.cy, c.old_y) + c.radius + 2.0
     AND GREATEST(b.y1, b.y2) >= LEAST(c.cy, c.old_y) - c.radius - 2.0
    CROSS JOIN LATERAL (
      SELECT LEAST(1.0, GREATEST(0.0,
        ((c.cx - b.x1) * (b.x2 - b.x1) + (c.cy - b.y1) * (b.y2 - b.y1))
        / NULLIF((b.x2 - b.x1) * (b.x2 - b.x1) + (b.y2 - b.y1) * (b.y2 - b.y1), 0.0)
      )) AS t
    ) lt
    CROSS JOIN LATERAL (
      -- PostgreSQL's ^ is a power; MySQL's is xor
      SELECT sqrt(POWER(c.cx - (b.x1 + lt.t * (b.x2 - b.x1)), 2) + POWER(c.cy - (b.y1 + lt.t * (b.y2 - b.y1)), 2)) AS dist
    ) dd
    CROSS JOIN LATERAL (
      SELECT
        (c.old_x - b.x1) * (b.y2 - b.y1) - (c.old_y - b.y1) * (b.x2 - b.x1) AS d1,
        (c.cx - b.x1) * (b.y2 - b.y1) - (c.cy - b.y1) * (b.x2 - b.x1) AS d2,
        (b.x1 - c.old_x) * (c.cy - c.old_y) - (b.y1 - c.old_y) * (c.cx - c.old_x) AS d3,
        (b.x2 - c.old_x) * (c.cy - c.old_y) - (b.y2 - c.old_y) * (c.cx - c.old_x) AS d4
    ) sides
    CROSS JOIN LATERAL (
      SELECT ((sides.d1 * sides.d2 < 0) AND (sides.d3 * sides.d4 < 0)) AS intersects
    ) xing
    GROUP BY c.pri, c.thing_id
  ),
  candidate_blocked AS (
    SELECT c.pri, c.thing_id, c.cx, c.cy, c.old_x, c.old_y, c.face_angle,
           c.dir, c.next_movecount,
           COALESCE(wb.blocked, FALSE) OR COALESCE(th2.hit, FALSE) AS blocked
    FROM candidates c
    LEFT JOIN wall_block wb ON wb.pri = c.pri AND wb.thing_id = c.thing_id
    LEFT JOIN thing_hit th2 ON th2.pri = c.pri AND th2.thing_id = c.thing_id
  ),
  best_pos AS (
    SELECT thing_id, cx, cy, old_x, old_y, face_angle, dir, next_movecount FROM (
      SELECT *, ROW_NUMBER() OVER (PARTITION BY thing_id ORDER BY pri) AS rn
      FROM candidate_blocked WHERE NOT blocked
    ) r WHERE rn = 1
  ),
  stuck AS (
    SELECT w.thing_id, w.mx, w.my, w.fresh_movecount
    FROM walk w
    WHERE NOT EXISTS (SELECT 1 FROM best_pos bp WHERE bp.thing_id = w.thing_id)
  )
  INSERT INTO monster_steps
    (map_id,thing_id,old_x,old_y,new_x,new_y,face_angle,movedir,movecount)
  SELECT a_map_id::int, bp.thing_id, bp.old_x, bp.old_y, bp.cx, bp.cy,
         bp.face_angle, bp.dir, GREATEST(0, bp.next_movecount)
  FROM best_pos bp
  UNION ALL
  SELECT a_map_id::int, s.thing_id, s.mx, s.my, s.mx, s.my, 0, NULL, s.fresh_movecount
  FROM stuck s;
  -- A_Chase's bookkeeping
  UPDATE monster_ai ai
  SET movedir = ms.movedir, movecount = ms.movecount
  FROM monster_steps ms
  WHERE ai.map_id = a_map_id AND ms.map_id = a_map_id
    AND ai.thing_id = ms.thing_id
    AND (ai.movedir IS DISTINCT FROM ms.movedir OR ai.movecount <> ms.movecount);
  SET v_stepped = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM monster_steps
                     WHERE map_id=a_map_id
                       AND (new_x <> old_x OR new_y <> old_y)) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_stepped = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_stepped;
END;;

-- Apply the staged poses, then the lines those moves crossed and the
-- teleports they landed on.
DROP FUNCTION IF EXISTS doom_cs_monster_move;;
CREATE FUNCTION doom_cs_monster_move(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- apply the staged pose
  UPDATE things t
  SET x = ms.new_x, y = ms.new_y, angle = ms.face_angle
  FROM monster_steps ms
  WHERE ms.map_id = a_map_id AND t.map_id = a_map_id AND t.id = ms.thing_id
    AND (ms.new_x <> ms.old_x OR ms.new_y <> ms.old_y);
  -- monster-crossable walk-over lines
  --  a monster trips only the teleports, the down-wait-up lifts and the plain raise-door, and nothing else.
  INSERT INTO line_special_events
    (map_id,player_thing_id,line_id,trigger_type,from_front)
  SELECT e.map_id,e.thing_id,e.line_id,'cross',e.from_front
  FROM (
    SELECT c.map_id, c.thing_id, c.line_id, c.from_front
    FROM (
      SELECT ms.map_id, ms.thing_id, ld.id AS line_id, ld.cross_once,
        ((v2.x-v1.x)::float8*(ms.old_y-v1.y)::float8
          - (v2.y-v1.y)::float8*(ms.old_x-v1.x)::float8) < 0 AS from_front,
        ABS((v2.x-v1.x)::float8*(ms.old_y-v1.y)::float8
          - (v2.y-v1.y)::float8*(ms.old_x-v1.x)::float8) AS old_side,
        ((v2.x-v1.x)::float8*(ms.new_y-v1.y)::float8
          - (v2.y-v1.y)::float8*(ms.new_x-v1.x)::float8) AS new_side,
        ((ms.new_x-ms.old_x)*(v1.y-ms.old_y)::float8
          - (ms.new_y-ms.old_y)*(v1.x-ms.old_x)::float8) AS v1_side,
        ((ms.new_x-ms.old_x)*(v2.y-ms.old_y)::float8
          - (ms.new_y-ms.old_y)*(v2.x-ms.old_x)::float8) AS v2_side
      FROM monster_steps ms
      JOIN linedefs ld ON ld.map_id = ms.map_id AND ld.monster_crossable
      JOIN vertexes v1 ON v1.map_id=ld.map_id AND v1.id=ld.v1_id
      JOIN vertexes v2 ON v2.map_id=ld.map_id AND v2.id=ld.v2_id
      WHERE ms.map_id = a_map_id
        AND (ms.new_x <> ms.old_x OR ms.new_y <> ms.old_y)
    ) c
    WHERE c.old_side > 1e-7
      AND c.new_side <> 0
      AND ((c.from_front AND c.new_side > 0)
        OR (NOT c.from_front AND c.new_side < 0))
      AND c.v1_side * c.v2_side < 0
      AND NOT (c.cross_once AND EXISTS (
        SELECT 1 FROM line_activations a
        WHERE a.map_id = c.map_id AND a.line_id = c.line_id))
  ) e
  ON CONFLICT (map_id,player_thing_id,line_id,trigger_type) DO NOTHING;
  -- EV_Teleport for a monster
  DELETE FROM monster_teleports WHERE map_id=a_map_id;
  INSERT INTO monster_teleports (map_id,thing_id,dest_x,dest_y,dest_angle,sector_id)
  SELECT map_id,thing_id,dest_x,dest_y,dest_angle,sector_id FROM (
    SELECT c.*, ROW_NUMBER() OVER
      (PARTITION BY c.map_id,c.thing_id ORDER BY c.off_centre, c.dest_id) AS rn
    FROM (
      SELECT t.map_id, t.thing_id, t.sector_id,
             th.id AS dest_id, th.x AS dest_x, th.y AS dest_y,
             th.angle AS dest_angle,
             ABS(th.x - (t.min_x+t.max_x)/2.0)
               + ABS(th.y - (t.min_y+t.max_y)/2.0) AS off_centre
      FROM (
        SELECT e.map_id, e.player_thing_id AS thing_id, s.id AS sector_id,
               MIN(rs.x1) AS min_x, MAX(rs.x1) AS max_x,
               MIN(rs.y1) AS min_y, MAX(rs.y1) AS max_y
        FROM line_special_events e
        JOIN linedefs ld ON ld.map_id=e.map_id AND ld.id=e.line_id
        JOIN line_special_defs d ON d.special=ld.special AND d.mechanic='teleport'
        -- Monster-owned events only: the player's id is never in monster_ai.
        JOIN monster_ai ai ON ai.map_id=e.map_id AND ai.thing_id=e.player_thing_id
        JOIN sectors s ON s.map_id=e.map_id AND s.tag=ld.tag
        JOIN render_segs rs ON rs.map_id=s.map_id AND rs.fsec=s.id
        WHERE e.map_id=a_map_id AND e.trigger_type='cross'
        GROUP BY e.map_id, e.player_thing_id, s.id
      ) t
      JOIN things th ON th.map_id=t.map_id
      JOIN thing_role_defs tr ON tr.thing_type=th.type AND tr.is_teleport_dest
       AND th.x BETWEEN t.min_x AND t.max_x
       AND th.y BETWEEN t.min_y AND t.max_y
    ) c
  ) q WHERE rn=1;
  -- sfx_telept at the spot left behind and at the one arrived at, as
  -- EV_Teleport's two MT_TFOGs do. The fog sprite itself is not modelled.
  INSERT INTO sound_events
    (map_id,event_key,level_tic,sound_name,source_thing_id,source_x,source_y)
  SELECT mt.map_id,'mon-tele-out:'||mt.thing_id::text||':'||ps.level_tics::text,
         ps.level_tics,'DSTELEPT',mt.thing_id,ms.old_x,ms.old_y
  FROM monster_teleports mt
  JOIN monster_steps ms ON ms.map_id=mt.map_id AND ms.thing_id=mt.thing_id
  JOIN player_state ps ON ps.map_id=mt.map_id
   AND ps.player_thing_id=a_player_thing_id::int
  WHERE mt.map_id=a_map_id
  UNION ALL
  SELECT mt.map_id,'mon-tele-in:'||mt.thing_id::text||':'||ps.level_tics::text,
         ps.level_tics,'DSTELEPT',mt.thing_id,mt.dest_x,mt.dest_y
  FROM monster_teleports mt
  JOIN player_state ps ON ps.map_id=mt.map_id
   AND ps.player_thing_id=a_player_thing_id::int
  WHERE mt.map_id=a_map_id
  ON CONFLICT (map_id,event_key) DO NOTHING;
  UPDATE things t
  SET x=mt.dest_x, y=mt.dest_y, angle=mt.dest_angle
  FROM monster_teleports mt
  WHERE t.map_id=a_map_id AND mt.map_id=a_map_id AND t.id=mt.thing_id;
  UPDATE monster_ai ai
  SET sector_id=mt.sector_id
  FROM monster_teleports mt
  WHERE ai.map_id=a_map_id AND mt.map_id=a_map_id AND ai.thing_id=mt.thing_id
    AND ai.sector_id IS DISTINCT FROM mt.sector_id;
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_float;;
CREATE FUNCTION doom_cs_monster_float(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_view_height DOUBLE;
  DECLARE v_player_height DOUBLE;
  DECLARE v_skull_speed DOUBLE;
  DECLARE v_skull_reach DOUBLE;
  DECLARE v_ticrate DOUBLE;
  SET v_view_height = doom_const('VIEWHEIGHT');
  SET v_player_height = doom_const('PLAYER_HEIGHT');
  SET v_skull_speed = doom_const('SKULL_CHARGE_SPEED');
  SET v_skull_reach = doom_const('SKULL_HIT_REACH');
  SET v_ticrate = doom_const('TICRATE');
  -- P_ZMovement's float
  -- A floater drifts 4 units a tic toward its target's mid-height once it is
  -- close enough horizontally (within three times the height difference), and
  -- stays between its sector's floor and the ceiling minus its own height.
  UPDATE things t
  SET z = LEAST(q.ceil_height - q.height, GREATEST(q.floor_height,
            t.z + CASE WHEN q.delta > 0 AND q.dist < q.delta * 3 THEN 4
                       WHEN q.delta < 0 AND q.dist < -q.delta * 3 THEN -4 ELSE 0 END))::real
  FROM (
    SELECT ai.thing_id, s.floor_height, s.ceil_height, d.height,
           ((ps.base_z - v_view_height) + v_player_height/2.0) - th.z AS delta,
           SQRT(POWER(pt.x - th.x, 2) + POWER(pt.y - th.y, 2)) AS dist
    FROM monster_ai ai
    JOIN things th ON th.map_id = ai.map_id AND th.id = ai.thing_id
    LEFT JOIN render_things rt ON rt.map_id = ai.map_id AND rt.thing_id = ai.thing_id
    JOIN sectors s ON s.map_id = ai.map_id AND s.id = COALESCE(ai.sector_id, rt.sector_id)
    JOIN player_state ps ON ps.map_id = ai.map_id
     AND ps.player_thing_id = COALESCE(ai.target_thing_id, a_player_thing_id::int)
    JOIN things pt ON pt.map_id = ps.map_id AND pt.id = ps.player_thing_id
    JOIN thing_combat_defs d ON d.thing_type = th.type
    WHERE ai.map_id = a_map_id AND d.floats AND ai.state IN ('see','missile')
  ) q
  WHERE t.map_id = a_map_id AND t.id = q.thing_id;
  -- A_SkullAttack
  -- The lost soul's attack frame hurls it at its target at 20 units a tic;
  -- doom_cs_thing_physics carries the momentum and stops it at walls, and the
  -- charge is over when it lands (below), hits a wall, or runs out.
  UPDATE things t
  SET mom_x = (v_skull_speed * (q.tx - t.x) / q.len)::real,
      mom_y = (v_skull_speed * (q.ty - t.y) / q.len)::real
  FROM (
    SELECT ai.thing_id, pt.x::float8 AS tx, pt.y::float8 AS ty,
           SQRT(POWER(pt.x - th.x, 2) + POWER(pt.y - th.y, 2)) AS len
    FROM monster_ai ai
    JOIN things th ON th.map_id = ai.map_id AND th.id = ai.thing_id
    JOIN things pt ON pt.map_id = ai.map_id
     AND pt.id = COALESCE(ai.target_thing_id, a_player_thing_id::int)
    JOIN thing_combat_defs d ON d.thing_type = th.type AND d.skull_fly
    WHERE ai.map_id = a_map_id AND ai.fired_this_tick
  ) q
  WHERE t.map_id = a_map_id AND t.id = q.thing_id AND q.len > 0;
  UPDATE monster_ai ai
  SET charge_tics = CASE WHEN ai.fired_this_tick THEN v_ticrate ELSE GREATEST(0, ai.charge_tics - 1) END
  FROM things t JOIN thing_combat_defs d ON d.thing_type = t.type AND d.skull_fly
  WHERE ai.map_id = a_map_id AND t.map_id = ai.map_id AND t.id = ai.thing_id
    AND (ai.fired_this_tick OR ai.charge_tics > 0);
  -- The charge lands: 3d8 to a player within reach, and the skull stops.
  UPDATE player_state ps
  SET armor = ps.armor - d.saved,
      armor_class = CASE WHEN ps.armor - d.saved <= 0 THEN 0 ELSE ps.armor_class END,
      health = GREATEST(0, ps.health - (d.dmg - d.saved)),
      damage_count = LEAST(100, ps.damage_count + (d.dmg - d.saved)),
      pain_face_tics = 12,
      alive = (ps.health - (d.dmg - d.saved)) > 0,
      killer_id = CASE WHEN (ps.health - (d.dmg - d.saved)) <= 0 THEN -1 ELSE ps.killer_id END
  FROM (
    SELECT ps2.map_id, ps2.player_thing_id, q.dmg,
           LEAST(ps2.armor, CASE ps2.armor_class WHEN 2 THEN q.dmg / 2 WHEN 1 THEN q.dmg / 3 ELSE 0 END) AS saved
    FROM player_state ps2
    JOIN (
      SELECT ps3.player_thing_id,
             SUM((doom_prandom(t.id, ps3.level_tics, 21) % d.charge_sides + 1)
                 * d.charge_mult)::int AS dmg
      FROM monster_ai ai
      JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
      JOIN thing_combat_defs d ON d.thing_type = t.type AND d.skull_fly
      JOIN player_state ps3 ON ps3.map_id = ai.map_id AND ps3.alive
      JOIN things pt ON pt.map_id = ps3.map_id AND pt.id = ps3.player_thing_id
      WHERE ai.map_id = a_map_id AND ai.charge_tics > 0
        AND ABS(pt.x - t.x) < v_skull_reach AND ABS(pt.y - t.y) < v_skull_reach
      GROUP BY ps3.player_thing_id
    ) q ON q.player_thing_id = ps2.player_thing_id
    WHERE ps2.map_id = a_map_id AND NOT ps2.god_mode AND ps2.invuln_tics <= 0
  ) d
  WHERE ps.map_id = d.map_id AND ps.player_thing_id = d.player_thing_id;
  UPDATE things t
  SET mom_x = 0, mom_y = 0
  FROM monster_ai ai JOIN thing_combat_defs d ON d.thing_type = t.type AND d.skull_fly
  WHERE t.map_id = a_map_id AND ai.map_id = t.map_id AND ai.thing_id = t.id
    AND ai.charge_tics > 0
    AND EXISTS (SELECT 1 FROM player_state ps JOIN things pt ON pt.map_id = ps.map_id AND pt.id = ps.player_thing_id
                WHERE ps.map_id = t.map_id AND ABS(pt.x - t.x) < v_skull_reach AND ABS(pt.y - t.y) < v_skull_reach);
  UPDATE monster_ai ai
  SET charge_tics = 0
  FROM things t JOIN thing_combat_defs d ON d.thing_type = t.type AND d.skull_fly
  WHERE ai.map_id = a_map_id AND t.map_id = ai.map_id AND t.id = ai.thing_id
    AND ai.charge_tics > 0 AND t.mom_x = 0 AND t.mom_y = 0 AND NOT ai.fired_this_tick;
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_chase;;
CREATE FUNCTION doom_cs_monster_chase(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- a blocked monster opens the door
  -- P_Move, when its step fails, retries the lines it hit as use-specials, and
  -- P_UseSpecialLine lets a monster through exactly four door types (1, 32, 33,
  -- 34 -- key doors included, since the key check is skipped for anything that
  -- is not a player). ML_SECRET lines are never opened.
  INSERT INTO line_special_events
    (map_id,player_thing_id,line_id,trigger_type,from_front)
  SELECT DISTINCT a_map_id::int, ai.thing_id, ld.linedef_id, 'use', TRUE
  FROM monster_ai ai
  JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
  JOIN thing_combat_defs cd ON cd.thing_type = t.type
  JOIN things pt ON pt.map_id = ai.map_id AND pt.id = a_player_thing_id::int
  JOIN linedef_geom ld ON ld.map_id = ai.map_id
  JOIN line_special_defs d ON d.special = ld.special AND d.monster_usable
  CROSS JOIN LATERAL (
    SELECT SQRT(POWER(pt.x - t.x, 2) + POWER(pt.y - t.y, 2))::double precision AS dist
  ) toward
  CROSS JOIN LATERAL (
    SELECT t.x + 8.0 * (pt.x - t.x) / NULLIF(toward.dist, 0) AS sx,
           t.y + 8.0 * (pt.y - t.y) / NULLIF(toward.dist, 0) AS sy
  ) step
  CROSS JOIN LATERAL (
    SELECT LEAST(1.0, GREATEST(0.0,
      ((step.sx - ld.x1) * (ld.x2 - ld.x1) + (step.sy - ld.y1) * (ld.y2 - ld.y1))
      / NULLIF(POWER(ld.x2 - ld.x1, 2) + POWER(ld.y2 - ld.y1, 2), 0.0))) AS u
  ) proj
  CROSS JOIN LATERAL (
    SELECT SQRT(POWER(step.sx - (ld.x1 + proj.u * (ld.x2 - ld.x1)), 2)
              + POWER(step.sy - (ld.y1 + proj.u * (ld.y2 - ld.y1)), 2)) AS gap
  ) near
  LEFT JOIN sidedefs back ON back.map_id = ld.map_id AND back.id = ld.left_sd_id
  LEFT JOIN sector_movers sm ON sm.map_id = ld.map_id
    AND sm.sector_id = back.sector_id AND sm.direction <> -1
  WHERE ai.map_id = a_map_id::int
    AND ai.state = 'see'
    AND NOT cd.explodes
    AND (ld.flags & 32) = 0
    AND near.gap < cd.radius
    AND sm.sector_id IS NULL
  ON CONFLICT (map_id,player_thing_id,line_id,trigger_type) DO NOTHING;
  -- live sector for monsters now in 'see'
  WITH RECURSIVE
  params AS (SELECT a_map_id::int AS map_id),
  movers AS (
    -- A monster's sector can only change on a tic it stepped, so only those
    -- descend the BSP again -- plus any chaser that has never been placed.
    -- Descending for every chaser was ~3/4 of this statement.
    SELECT ai.thing_id, t.x::double precision AS mx, t.y::double precision AS my
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    JOIN things t ON t.map_id = ai.map_id AND t.id = ai.thing_id
    WHERE ai.state = 'see'
      AND (ai.sector_id IS NULL
           OR EXISTS (SELECT 1 FROM monster_steps ms
                      WHERE ms.map_id = ai.map_id AND ms.thing_id = ai.thing_id))
  ),
  bsp AS (
    SELECT m.thing_id, 1 AS depth, n.id AS node_id, NULL::int AS ssec_id
    FROM movers m
    CROSS JOIN LATERAL (
      SELECT n.id FROM nodes n JOIN params p ON n.map_id = p.map_id
      ORDER BY n.id DESC LIMIT 1
    ) n
    UNION ALL
    SELECT w.thing_id, w.depth + 1,
      CASE WHEN nc.child_kind = 'NODE' THEN nc.child_node_id END,
      CASE WHEN nc.child_kind = 'SSECTOR' THEN nc.child_ssector_id END
    FROM bsp w
    JOIN params p ON TRUE
    JOIN movers m ON m.thing_id = w.thing_id
    JOIN nodes n ON n.map_id = p.map_id AND n.id = w.node_id
    JOIN node_children nc ON nc.map_id = p.map_id AND nc.node_id = n.id
      AND nc.side = CASE
        WHEN (m.mx - n.x)::float8 * n.dy::float8
             - (m.my - n.y)::float8 * n.dx::float8 > 0
        THEN 'R' ELSE 'L' END
    WHERE w.node_id IS NOT NULL
  ),
  subsector AS (
    SELECT thing_id, ssec_id FROM (
      SELECT thing_id, ssec_id, ROW_NUMBER() OVER
        (PARTITION BY thing_id ORDER BY depth, ssec_id) AS rn
      FROM bsp WHERE ssec_id IS NOT NULL
    ) r WHERE rn = 1
  ),
  sector_of AS (
    SELECT ss.thing_id, s.id AS sector_id,
      ROW_NUMBER() OVER (PARTITION BY ss.thing_id ORDER BY seg.id) AS rn
    FROM subsector ss
    JOIN params p ON TRUE
    JOIN ssectors sst ON sst.map_id = p.map_id AND sst.id = ss.ssec_id
    JOIN segs seg ON seg.map_id = p.map_id
      AND seg.id >= sst.first_seg_id AND seg.id < sst.first_seg_id + sst.seg_count
    JOIN linedefs ld ON ld.map_id = p.map_id AND ld.id = seg.linedef_id
    JOIN sidedefs sd ON sd.map_id = p.map_id
      AND sd.id = CASE WHEN seg.direction = 0 THEN ld.right_sd_id ELSE ld.left_sd_id END
    JOIN sectors s ON s.map_id = p.map_id AND s.id = sd.sector_id
  )
  UPDATE monster_ai ai
  SET sector_id = so.sector_id
  FROM sector_of so
  WHERE so.rn = 1 AND ai.map_id = a_map_id AND ai.thing_id = so.thing_id
    -- A monster that did not move is still in the sector it was in
    AND ai.sector_id IS DISTINCT FROM so.sector_id;
  -- keep the render cache's sector in step
  UPDATE render_things rt
  SET sector_id = ai.sector_id
  FROM monster_ai ai
  WHERE rt.map_id = a_map_id AND ai.map_id = a_map_id
    AND ai.thing_id = rt.thing_id
    AND ai.sector_id IS NOT NULL
    AND rt.sector_id <> ai.sector_id;
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_attack;;
CREATE FUNCTION doom_cs_monster_attack(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_attack_range_default DOUBLE;
  DECLARE v_hitscan_spread DOUBLE;
  DECLARE v_shadow_miss DOUBLE;
  DECLARE v_melee_reach DOUBLE;
  DECLARE v_player_radius DOUBLE;
  SET v_attack_range_default = doom_const('DEFAULT_ATTACK_RANGE');
  SET v_hitscan_spread = doom_const('HITSCAN_SPREAD_UNITS');
  SET v_shadow_miss = doom_const('SHADOW_MISS_UNITS');
  SET v_melee_reach = doom_const('MELEE_REACH');
  SET v_player_radius = doom_const('PLAYER_RADIUS');
  DELETE FROM monster_attack_damage WHERE map_id = a_map_id;
  WITH
  params AS (SELECT a_map_id::int AS map_id, a_player_thing_id::int AS player_thing_id),
  player_pos AS (
    SELECT pp.px, pp.py
    FROM doom_player_pos pp JOIN params p ON pp.map_id = p.map_id
     AND pp.player_thing_id = p.player_thing_id
  ),
  -- Every live player on the map. A monster attacks the one it is angry with
  -- (mon_target below); a barrel's blast reaches all of them.
  players AS (
    SELECT ps.player_thing_id, t.x::double precision AS px, t.y::double precision AS py
    FROM player_state ps JOIN params p ON ps.map_id = p.map_id
    JOIN things t ON t.map_id = ps.map_id AND t.id = ps.player_thing_id
    WHERE ps.alive
  ),
  targets AS (
    SELECT player_thing_id, px, py FROM players
    UNION ALL
    SELECT p.player_thing_id, pp.px, pp.py
    FROM params p CROSS JOIN player_pos pp
    WHERE NOT EXISTS (SELECT 1 FROM players q
                      WHERE q.player_thing_id = p.player_thing_id)
  ),
  -- MF_SHADOW on the target, which for the player means the blur sphere.
  shadow AS (
    SELECT sh.shadowed, sh.level_tics
    FROM doom_player_shadow sh JOIN params p ON sh.map_id = p.map_id
     AND sh.player_thing_id = p.player_thing_id
  ),
  attackers AS (
    SELECT a.thing_id, a.type, a.mx, a.my,
           a.hitscan_pellets, a.hitscan_mult, a.hitscan_sides,
           a.melee_mult, a.melee_sides, a.explodes,
           a.blast_radius
    FROM doom_monster_attackers a JOIN params p ON a.map_id = p.map_id
  ),
  blocking_walls AS (
    SELECT ld.x1::double precision AS x1, ld.y1::double precision AS y1,
           ld.x2::double precision AS x2, ld.y2::double precision AS y2
    FROM linedef_geom ld JOIN params p ON ld.map_id = p.map_id
    LEFT JOIN sectors fr ON fr.map_id = ld.map_id AND fr.id = ld.fsec
    LEFT JOIN sectors bk ON bk.map_id = ld.map_id AND bk.id = ld.bsec
    -- Recheck live portal geometry on the attack frame. This prevents a shot
    -- already being animated from damaging the player after its door closes.
    WHERE ld.left_sd_id = -1 OR ld.right_sd_id = -1
       OR fr.id IS NULL OR bk.id IS NULL
       OR LEAST(fr.ceil_height, bk.ceil_height)
            <= GREATEST(fr.floor_height, bk.floor_height)
  ),
  mon_target AS (
    SELECT ai.thing_id,
           COALESCE(tt.x::double precision, pp.px) AS tx,
           COALESCE(tt.y::double precision, pp.py) AS ty,
           CASE WHEN th.thing_id IS NULL THEN NULL ELSE ai.target_thing_id END
             AS victim_id,
           COALESCE(tp.player_thing_id, pp.player_thing_id) AS victim_player
    FROM monster_ai ai
    JOIN params p ON ai.map_id = p.map_id
    JOIN things at ON at.map_id = ai.map_id AND at.id = ai.thing_id
    JOIN thing_combat_defs acd ON acd.thing_type = at.type
    JOIN targets pp ON (acd.explodes OR pp.player_thing_id = p.player_thing_id)
    LEFT JOIN things tt ON tt.map_id = ai.map_id AND tt.id = ai.target_thing_id
    LEFT JOIN thing_health th ON th.map_id = ai.map_id
     AND th.thing_id = ai.target_thing_id AND th.alive
    LEFT JOIN player_state tp ON tp.map_id = ai.map_id
     AND tp.player_thing_id = ai.target_thing_id AND tp.alive
  ),
  hits AS (
    SELECT a.thing_id, a.type, a.hitscan_pellets, a.hitscan_mult, a.hitscan_sides,
      a.melee_mult, a.melee_sides, a.explodes,
      a.blast_radius,
      mt.victim_id, mt.victim_player, sh.level_tics AS tic,
      sqrt(power(a.mx - pp.px, 2) + power(a.my - pp.py, 2)) AS dist,
      NOT EXISTS (
        SELECT 1 FROM blocking_walls b
        CROSS JOIN LATERAL (
          SELECT
            (a.mx - b.x1) * (b.y2 - b.y1) - (a.my - b.y1) * (b.x2 - b.x1) AS d1,
            (pp.px - b.x1) * (b.y2 - b.y1) - (pp.py - b.y1) * (b.x2 - b.x1) AS d2,
            (b.x1 - a.mx) * (pp.py - a.my) - (b.y1 - a.my) * (pp.px - a.mx) AS d3,
            (b.x2 - a.mx) * (pp.py - a.my) - (b.y2 - a.my) * (pp.px - a.mx) AS d4
        ) sides
        WHERE (sides.d1 * sides.d2 < 0) AND (sides.d3 * sides.d4 < 0)
      ) AS visible
    FROM attackers a
    CROSS JOIN shadow sh
    JOIN mon_target mt ON mt.thing_id = a.thing_id
    CROSS JOIN LATERAL (SELECT mt.tx AS px, mt.ty AS py) pp
  ),
  pellet_random AS (
    SELECT h.thing_id,h.type,h.victim_id,h.victim_player,h.dist,h.visible,g.pellet,
           doom_prandom(h.thing_id, h.tic * 8 + g.pellet, 12) AS r1,
           doom_prandom(h.thing_id, h.tic * 8 + g.pellet, 13) AS r2,
           (doom_prandom(h.thing_id, h.tic * 8 + g.pellet, 14)
              % h.hitscan_sides + 1) * h.hitscan_mult
             AS pellet_damage,

           CASE WHEN sh.shadowed
                THEN (doom_prandom(h.thing_id, h.tic, 15)
                     -doom_prandom(h.thing_id, h.tic, 16))*360.0/v_shadow_miss
                ELSE 0.0 END AS shadow_err
    FROM hits h
    CROSS JOIN shadow sh
    CROSS JOIN LATERAL generate_series(1, h.hitscan_pellets) g(pellet)
    WHERE h.hitscan_pellets IS NOT NULL
  ),
  damage AS (
    SELECT thing_id, victim_id, victim_player,
      CASE WHEN visible AND dist<=v_attack_range_default
             AND (dist<=v_player_radius OR
               ABS((r1-r2)*360.0/v_hitscan_spread + shadow_err)
                 <=DEGREES(ASIN(LEAST(1.0,v_player_radius/NULLIF(dist,0.0)))))
           THEN pellet_damage ELSE 0 END AS dmg
    FROM pellet_random
    UNION ALL
    SELECT thing_id, victim_id, victim_player,
      CASE
        -- The bite at MELEERANGE (A_TroopAttack, A_SargAttack, A_HeadAttack,
        -- A_BruisAttack): melee_mult * (1..melee_sides) from the catalogue. A
        -- line attack, so the thing has to see its target.
        WHEN melee_mult IS NOT NULL AND visible AND dist<=v_melee_reach
          THEN (doom_prandom(thing_id, tic, 17) % melee_sides + 1)*melee_mult
        -- The barrel's blast falls off with distance.
        WHEN explodes AND visible AND dist<blast_radius
          THEN GREATEST(0,blast_radius-FLOOR(dist)::int)::int
        ELSE 0
      END AS dmg
    FROM hits WHERE melee_mult IS NOT NULL OR explodes
  )
  INSERT INTO monster_attack_damage
    (map_id, attacker_id, victim_id, victim_player, dmg, mx, my)
  SELECT a_map_id::int, d.thing_id, d.victim_id, d.victim_player, d.dmg,
         a.mx, a.my
  FROM damage d
  JOIN attackers a ON a.thing_id = d.thing_id;
  -- the staged rolls landing on a player
  WITH
  params AS (SELECT a_map_id::int AS map_id, a_player_thing_id::int AS player_thing_id),
  -- Every live player on the map. The shove joins this, so a roll staged
  -- against a player who has since died lands on nobody.
  players AS (
    SELECT ps.player_thing_id, t.x::double precision AS px, t.y::double precision AS py
    FROM player_state ps JOIN params p ON ps.map_id = p.map_id
    JOIN things t ON t.map_id = ps.map_id AND t.id = ps.player_thing_id
    WHERE ps.alive
  ),
  -- P_DamageMobj's `if (player && gameskill == sk_baby) damage >>= 1`, which
  -- runs per hit and before the armour absorbs its third or half. Monster
  -- victims (victim_id NOT NULL, the infighting path) never get it.
  hurt AS (
    SELECT d.dmg >> CASE WHEN g.skill=0 THEN 1 ELSE 0 END AS dmg,
           d.mx, d.my, d.victim_player
    FROM monster_attack_damage d
    JOIN params p ON d.map_id = p.map_id
    JOIN game_tic_commands g ON g.map_id = p.map_id
     AND g.player_thing_id = d.victim_player
    WHERE d.victim_id IS NULL
  ),
  -- P_DamageMobj shoves the player exactly as it shoves a monster:
  -- `thrust = damage*(FRACUNIT>>3)*100/mass` directed away from the inflictor,
  -- which for a bullet or a claw is the attacker itself. MT_PLAYER's mass is
  -- 100, so the shove is damage/8 map units per tic. It is computed from the
  -- damage before the armour absorbs any of it, and it is summed per hit --
  -- three shotgun pellets from the same sergeant push three times as hard as
  -- one.
  shove AS (
    SELECT h.dmg, h.victim_player,
           CASE WHEN r.dist > 0 THEN h.dmg * 0.125 * r.ddx / r.dist
                ELSE 0.0 END AS tx,
           CASE WHEN r.dist > 0 THEN h.dmg * 0.125 * r.ddy / r.dist
                ELSE 0.0 END AS ty
    FROM hurt h
    JOIN players pp ON pp.player_thing_id = h.victim_player
    CROSS JOIN LATERAL (
      SELECT pp.px - h.mx AS ddx, pp.py - h.my AS ddy,
             sqrt(power(pp.px - h.mx, 2) + power(pp.py - h.my, 2)) AS dist
    ) r
  ),
  totaled AS (
    SELECT a_map_id::int AS map_id, victim_player AS player_thing_id,
           SUM(dmg)::int AS total_dmg,
           SUM(FLOOR(dmg/3.0))::int AS green_saved,
           SUM(FLOOR(dmg/2.0))::int AS blue_saved,
           -- 16.16 fixed point, as vanilla accumulates momentum: a float sum of
           -- several hits would depend on the order the rows arrive in.
           SUM(ROUND(tx * 65536.0)) / 65536.0 AS thrust_x,
           SUM(ROUND(ty * 65536.0)) / 65536.0 AS thrust_y
    FROM shove
    GROUP BY victim_player
  ),
  absorbed AS (
    SELECT t.*,
           (ps.god_mode OR ps.invuln_tics > 0) AS blocked,
           CASE WHEN ps.god_mode OR ps.invuln_tics > 0 THEN 0
                ELSE LEAST(ps.armor, CASE ps.armor_class
                       WHEN 2 THEN t.blue_saved WHEN 1 THEN t.green_saved
                       ELSE 0 END)
           END AS saved
    FROM totaled t
    JOIN player_state ps ON ps.map_id = t.map_id AND ps.player_thing_id = t.player_thing_id
  ),
  applied AS (
    SELECT a.*, CASE WHEN a.blocked THEN 0 ELSE a.total_dmg - a.saved END AS took
    FROM absorbed a
  )
  UPDATE player_state ps
  SET armor = ps.armor - a.saved,
      armor_class=CASE WHEN ps.armor-a.saved<=0 THEN 0 ELSE ps.armor_class END,
      health = GREATEST(0, ps.health - a.took),
      -- P_DamageMobj: player->damagecount += damage, which reddens the screen.
      damage_count = LEAST(100, ps.damage_count + a.took),
      alive = GREATEST(0, ps.health - a.took) > 0,
      -- Renderer shows the STFOUCH pain face while this is > 0; a fresh hit
      -- always resets it to a full flash.
      pain_face_tics = CASE WHEN a.took > 0 THEN 12
                            ELSE ps.pain_face_tics END,
      -- A monster or a barrel is the world, for the deathmatch scoreboard.
      killer_id = CASE WHEN ps.alive AND ps.health - a.took <= 0 THEN -1
                       ELSE ps.killer_id END,
      momentum_x = (ps.momentum_x + a.thrust_x)::real,
      momentum_y = (ps.momentum_y + a.thrust_y)::real
  FROM applied a
  WHERE ps.map_id = a.map_id AND ps.player_thing_id = a.player_thing_id;
  -- the same rolls, landing on a monster
  UPDATE thing_health h
  SET health = h.health - v.dmg, alive = h.health - v.dmg > 0
  FROM (
    SELECT victim_id, SUM(dmg)::int AS dmg
    FROM monster_attack_damage
    WHERE map_id = a_map_id::int AND victim_id IS NOT NULL
      AND victim_player = a_player_thing_id::int
    GROUP BY victim_id
  ) v
  WHERE h.map_id = a_map_id::int AND h.thing_id = v.victim_id AND v.dmg > 0;
  -- the victim turns on whoever shot it
  UPDATE monster_ai ai
  SET target_thing_id = v.attacker,
      state = CASE WHEN ai.state = 'stand' THEN 'see'::actor_state ELSE ai.state END,
      seq_index = CASE WHEN ai.state = 'stand' THEN 0 ELSE ai.seq_index END,
      state_tics = CASE WHEN ai.state = 'stand' THEN 0 ELSE ai.state_tics END
  FROM (
    SELECT victim_id, MIN(attacker_id) AS attacker
    FROM monster_attack_damage
    WHERE map_id = a_map_id::int AND victim_id IS NOT NULL AND dmg > 0
      AND victim_player = a_player_thing_id::int
    GROUP BY victim_id
  ) v
  WHERE ai.map_id = a_map_id::int AND ai.thing_id = v.victim_id
    AND ai.thing_id <> v.attacker
    AND ai.target_thing_id IS DISTINCT FROM v.attacker
    AND EXISTS (SELECT 1 FROM thing_health th
                WHERE th.map_id = ai.map_id AND th.thing_id = ai.thing_id
                  AND th.alive);
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_barrel;;
CREATE FUNCTION doom_cs_monster_barrel(a_map_id INT) RETURNS INT
BEGIN
  -- exploding-barrel radius damage
  WITH
  params AS (SELECT a_map_id::int AS map_id),
  explosions AS (
    SELECT t.id AS source_id, t.x::double precision AS ex,
           t.y::double precision AS ey
    FROM monster_ai ai
    JOIN params p ON ai.map_id=p.map_id
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type AND cd.explodes
    WHERE ai.fired_this_tick
  ),
  blast AS (SELECT MAX(blast_radius) AS r FROM thing_combat_defs WHERE explodes),
  victims AS (
    SELECT DISTINCT h.map_id,h.thing_id,
           t.x::double precision AS vx,t.y::double precision AS vy
    FROM explosions e
    JOIN params p ON TRUE
    CROSS JOIN blast bl
    JOIN things t ON t.map_id=p.map_id
      AND t.x BETWEEN e.ex-bl.r AND e.ex+bl.r
      AND t.y BETWEEN e.ey-bl.r AND e.ey+bl.r
    JOIN thing_health h ON h.map_id=t.map_id AND h.thing_id=t.id AND h.alive
  ),
  blocking_walls AS (
    SELECT DISTINCT ld.x1::double precision AS x1,ld.y1::double precision AS y1,
           ld.x2::double precision AS x2,ld.y2::double precision AS y2
    FROM explosions e
    JOIN params p ON TRUE
    CROSS JOIN blast bl
    JOIN linedef_geom ld ON ld.map_id=p.map_id
    LEFT JOIN sectors fr ON fr.map_id=ld.map_id AND fr.id=ld.fsec
    LEFT JOIN sectors bk ON bk.map_id=ld.map_id AND bk.id=ld.bsec
    WHERE (ld.left_sd_id=-1 OR ld.right_sd_id=-1
       OR fr.id IS NULL OR bk.id IS NULL
       OR LEAST(fr.ceil_height,bk.ceil_height)
            <= GREATEST(fr.floor_height,bk.floor_height))
      -- Both endpoints of any blocked sight line lie within 128 of the blast,
      -- so a wall that can block one has to reach into this box.
      AND LEAST(ld.x1,ld.x2) <= e.ex+bl.r
      AND GREATEST(ld.x1,ld.x2) >= e.ex-bl.r
      AND LEAST(ld.y1,ld.y2) <= e.ey+bl.r
      AND GREATEST(ld.y1,ld.y2) >= e.ey-bl.r
  ),
  radial AS (
    SELECT v.map_id,v.thing_id,
           SUM(GREATEST(0,bl.r-FLOOR(d.dist)::int)::int)::int AS damage
    FROM explosions e CROSS JOIN victims v CROSS JOIN blast bl
    CROSS JOIN LATERAL (
      SELECT SQRT(POWER(v.vx-e.ex,2)+POWER(v.vy-e.ey,2)) AS dist
    ) d
    WHERE v.thing_id<>e.source_id AND d.dist<bl.r
      AND NOT EXISTS (
        SELECT 1 FROM blocking_walls b
        CROSS JOIN LATERAL (
          SELECT
            (e.ex-b.x1)*(b.y2-b.y1)-(e.ey-b.y1)*(b.x2-b.x1) AS d1,
            (v.vx-b.x1)*(b.y2-b.y1)-(v.vy-b.y1)*(b.x2-b.x1) AS d2,
            (b.x1-e.ex)*(v.vy-e.ey)-(b.y1-e.ey)*(v.vx-e.ex) AS d3,
            (b.x2-e.ex)*(v.vy-e.ey)-(b.y2-e.ey)*(v.vx-e.ex) AS d4
        ) sides
        WHERE sides.d1*sides.d2<0 AND sides.d3*sides.d4<0
      )
    GROUP BY v.map_id,v.thing_id
  )
  UPDATE thing_health h
  SET health=h.health-r.damage,alive=h.health-r.damage>0
  FROM radial r
  WHERE h.map_id=r.map_id AND h.thing_id=r.thing_id;
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_blast;;
CREATE FUNCTION doom_cs_monster_blast(a_map_id INT) RETURNS INT
BEGIN
  -- P_DamageMobj's thrust
  -- The last thing P_DamageMobj does is shove the target away from whatever hurt
  -- it: `thrust = damage*(FRACUNIT>>3)*100/mass`, which is damage*12.5/mass map
  -- units a tic, along the angle from the inflictor to the target. It is what
  -- makes one barrel throw the next -- and, through the same statement, what
  -- makes a rocket knock a trooper back.
  WITH
  params AS (SELECT a_map_id::int AS map_id),
  explosions AS (
    SELECT i.projectile_id, mp.owner_thing_id AS source_id,
           i.x::double precision AS ex, i.y::double precision AS ey
    FROM projectile_impacts i
    JOIN monster_projectiles mp ON mp.map_id=i.map_id
     AND mp.projectile_id=i.projectile_id
    JOIN params p ON i.map_id=p.map_id
    WHERE i.projectile_type='rocket'
    UNION ALL
    SELECT -ai.thing_id, ai.thing_id, t.x::double precision, t.y::double precision
    FROM monster_ai ai
    JOIN params p ON ai.map_id=p.map_id
    JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
    JOIN thing_combat_defs cd ON cd.thing_type=t.type AND cd.explodes
    WHERE ai.fired_this_tick
  ),
  shoved AS (
    SELECT v.id AS thing_id,
           SUM(ROUND(GREATEST(0,128-FLOOR(d.dist)::int) * 12.5
               / GREATEST(1,cd.mass) * d.dx / NULLIF(d.dist,0) * 65536.0)) / 65536.0 AS push_x,
           SUM(ROUND(GREATEST(0,128-FLOOR(d.dist)::int) * 12.5
               / GREATEST(1,cd.mass) * d.dy / NULLIF(d.dist,0) * 65536.0)) / 65536.0 AS push_y
    FROM explosions e
    JOIN things v ON v.map_id=a_map_id::int
    JOIN thing_combat_defs cd ON cd.thing_type=v.type
    CROSS JOIN LATERAL (
      SELECT v.x-e.ex AS dx, v.y-e.ey AS dy,
             SQRT(POWER(v.x-e.ex,2)+POWER(v.y-e.ey,2)) AS dist
    ) d
    WHERE v.id<>e.source_id AND d.dist<128.0 AND d.dist>0.0
    GROUP BY v.id
  )
  UPDATE things t
  SET mom_x = (t.mom_x + s.push_x)::real,
      mom_y = (t.mom_y + s.push_y)::real
  FROM shoved s
  WHERE t.map_id=a_map_id::int AND t.id=s.thing_id;
  RETURN 0;
END;;

-- Record corpses that have settled and pick this tic's P_NightmareRespawn
-- set. Returns whether anything was picked, which gates doom_cs_monster_respawn.
DROP FUNCTION IF EXISTS doom_cs_monster_deaths;;
CREATE FUNCTION doom_cs_monster_deaths(a_map_id INT, a_player_thing_id INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_monster_respawn_tics DOUBLE;
  DECLARE v_any_respawn BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_monster_respawn_tics = doom_const('MONSTER_RESPAWN_TICS');
  -- record when a corpse settled
  INSERT INTO monster_deaths (map_id,thing_id,death_tic)
  SELECT ai.map_id,ai.thing_id,ps.level_tics
  FROM monster_ai ai
  JOIN things t ON t.map_id=ai.map_id AND t.id=ai.thing_id
  JOIN thing_combat_defs d ON d.thing_type=t.type AND d.counts_kill
  JOIN player_state ps ON ps.map_id=ai.map_id
    AND ps.player_thing_id=a_player_thing_id::int
  WHERE ai.map_id=a_map_id::int AND ai.state='dead'
  ON CONFLICT (map_id,thing_id) DO NOTHING;
  -- choose this tic's P_NightmareRespawn set
  -- movecount >= 12*TICRATE, then a check only on every 32nd tic, then
  -- P_Random() <= 4 -- five values in 256, rolled per corpse. Staged into a
  -- table because the statements below all have to agree on the same set.
  DELETE FROM monster_respawns WHERE map_id=a_map_id::int;
  INSERT INTO monster_respawns (map_id,thing_id,old_x,old_y)
  SELECT md.map_id,md.thing_id,t.x,t.y
  FROM monster_deaths md
  JOIN things t ON t.map_id=md.map_id AND t.id=md.thing_id
  JOIN player_state ps ON ps.map_id=md.map_id
    AND ps.player_thing_id=a_player_thing_id::int
  WHERE md.map_id=a_map_id::int
    AND ((SELECT g.skill FROM game_tic_commands g
               WHERE g.map_id=a_map_id::int
                 AND g.player_thing_id=a_player_thing_id::int)=4
         -- -respawn: the same rule on any skill
         OR EXISTS (SELECT 1 FROM mp_match m WHERE m.map_id=a_map_id::int AND m.respawn_monsters))
    AND ps.level_tics - md.death_tic >= v_monster_respawn_tics
    AND (ps.level_tics & 31) = 0
    AND doom_prandom(md.thing_id, ps.level_tics, 40) <= 4
    -- P_CheckPosition: no respawn while something else is standing on the spot.
    AND NOT EXISTS (
      SELECT 1
      FROM thing_health oh
      JOIN things ot ON ot.map_id=oh.map_id AND ot.id=oh.thing_id
      JOIN thing_combat_defs od ON od.thing_type=ot.type
      JOIN thing_combat_defs md2 ON md2.thing_type=t.type
      WHERE oh.map_id=md.map_id AND oh.alive AND oh.thing_id<>md.thing_id
        AND POWER(ot.x-t.spawn_x,2)+POWER(ot.y-t.spawn_y,2)
              < POWER(od.radius+md2.radius,2)
    );
  SET v_any_respawn = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM monster_respawns WHERE map_id=a_map_id) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_any_respawn = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_any_respawn;
END;;

DROP FUNCTION IF EXISTS doom_cs_monster_respawn;;
CREATE FUNCTION doom_cs_monster_respawn(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_respawn_fog_base DOUBLE;
  DECLARE v_tic_span DOUBLE;
  SET v_respawn_fog_base = doom_const('MONSTER_RESPAWN_EFFECT_ID_BASE');
  SET v_tic_span = doom_const('EFFECT_ID_TIC_SPAN');
  -- teleport fog, old spot and new
  -- P_NightmareRespawn spawns an MT_TFOG at each end and starts sfx_telept on
  -- both.
  INSERT INTO world_effects (map_id, effect_id, effect_type, x, y, z, sector_id, age)
  SELECT r.map_id, v_respawn_fog_base::bigint + r.thing_id::bigint * v_tic_span * 2
                   + (ps.level_tics % v_tic_span) * 2 + e.k,
         'tfog', CASE e.k WHEN 0 THEN r.old_x ELSE t.spawn_x::real END,
         CASE e.k WHEN 0 THEN r.old_y ELSE t.spawn_y::real END,
         s.floor_height, CASE e.k WHEN 0 THEN rt.sector_id ELSE rt.spawn_sector_id END, 0
  FROM monster_respawns r
  JOIN things t ON t.map_id=r.map_id AND t.id=r.thing_id
  JOIN render_things rt ON rt.map_id=r.map_id AND rt.thing_id=r.thing_id
  JOIN player_state ps ON ps.map_id=r.map_id AND ps.player_thing_id=a_player_thing_id::int
  CROSS JOIN (VALUES (0), (1)) AS e (k)
  JOIN sectors s ON s.map_id=r.map_id
   AND s.id=CASE e.k WHEN 0 THEN rt.sector_id ELSE COALESCE(rt.spawn_sector_id, rt.sector_id) END
  WHERE r.map_id=a_map_id::int
  ON CONFLICT (map_id, effect_id) DO NOTHING;
  INSERT INTO sound_events
    (map_id,event_key,level_tic,sound_name,source_thing_id,source_x,source_y)
  SELECT r.map_id,'respawn-out:'||r.thing_id::text||':'||ps.level_tics::text,
         ps.level_tics,'DSTELEPT',r.thing_id,r.old_x,r.old_y
  FROM monster_respawns r
  JOIN player_state ps ON ps.map_id=r.map_id
    AND ps.player_thing_id=a_player_thing_id::int
  WHERE r.map_id=a_map_id::int
  UNION ALL
  SELECT r.map_id,'respawn-in:'||r.thing_id::text||':'||ps.level_tics::text,
         ps.level_tics,'DSTELEPT',r.thing_id,t.spawn_x::real,t.spawn_y::real
  FROM monster_respawns r
  JOIN things t ON t.map_id=r.map_id AND t.id=r.thing_id
  JOIN player_state ps ON ps.map_id=r.map_id
    AND ps.player_thing_id=a_player_thing_id::int
  WHERE r.map_id=a_map_id::int
  ON CONFLICT (map_id,event_key) DO NOTHING;
  -- Its sight and death cues are latched by a stable per-monster key, so they
  -- have to be released or the monster comes back mute for the rest of the map.
  DELETE FROM sound_events e
  USING monster_respawns r
  WHERE e.map_id=r.map_id AND r.map_id=a_map_id::int
    AND e.event_key IN ('mon-sight:'||r.thing_id::text,
                        'mon-death:'||r.thing_id::text);
  -- put the monster back
  UPDATE things t
  SET x=t.spawn_x, y=t.spawn_y, angle=t.spawn_angle
  FROM monster_respawns r
  WHERE t.map_id=r.map_id AND t.id=r.thing_id AND r.map_id=a_map_id::int;
  UPDATE thing_health h
  SET health=h.max_health, alive=TRUE
  FROM monster_respawns r
  WHERE h.map_id=r.map_id AND h.thing_id=r.thing_id AND r.map_id=a_map_id::int;
  -- Back to sleep at the spawn point, exactly as P_SpawnMobj leaves it
  UPDATE render_things rt
  SET sector_id=rt.spawn_sector_id
  FROM monster_respawns r
  WHERE rt.map_id=r.map_id AND rt.thing_id=r.thing_id AND r.map_id=a_map_id::int
    AND rt.spawn_sector_id IS NOT NULL AND rt.sector_id<>rt.spawn_sector_id;
  UPDATE monster_ai ai
  SET state='stand', state_tics=-1, seq_index=0, attack_cooldown=18,
      fired_this_tick=FALSE, sector_id=rt.sector_id
  FROM monster_respawns r
  JOIN render_things rt ON rt.map_id=r.map_id AND rt.thing_id=r.thing_id
  WHERE ai.map_id=r.map_id AND ai.thing_id=r.thing_id AND r.map_id=a_map_id::int;
  DELETE FROM monster_deaths d
  USING monster_respawns r
  WHERE d.map_id=r.map_id AND d.thing_id=r.thing_id AND r.map_id=a_map_id::int;
  RETURN 0;
END;;

-- The pipeline. Nine of the eleven stages used to open with an EXISTS of
-- their own; doom_cs_monster_plan answers all of them in two statements.
DROP FUNCTION IF EXISTS doom_cs_monsters;;
CREATE FUNCTION doom_cs_monsters(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_pre BIGINT;
  DECLARE v_plan BIGINT;
  DECLARE v_stepped BOOLEAN;
  DECLARE v_respawning BOOLEAN;
  SET v_pre = doom_cs_monster_plan(a_map_id);
  DO doom_cs_monster_retarget(a_map_id,a_player_thing_id);
  IF (v_pre & 1) <> 0 THEN
    DO doom_cs_monster_noise(a_map_id);
  END IF;
  DO doom_cs_monster_think(a_map_id,a_player_thing_id);
  -- Re-planned: every bit below is a question about the state the think stage
  -- has just written.
  SET v_plan = doom_cs_monster_plan(a_map_id);
  -- Last tic's staging is cleared whether or not anyone steps this tic, or the
  -- move stage would replay it.
  DELETE FROM monster_steps WHERE map_id=a_map_id;
  SET v_stepped = false;
  IF (v_plan & 2) <> 0 THEN
    SET v_stepped = doom_cs_monster_step(a_map_id,a_player_thing_id);
  END IF;
  IF v_stepped THEN
    DO doom_cs_monster_move(a_map_id,a_player_thing_id);
  END IF;
  IF (v_plan & 4) <> 0 THEN
    DO doom_cs_monster_float(a_map_id,a_player_thing_id);
  END IF;
  IF (v_plan & 8) <> 0 THEN
    DO doom_cs_monster_chase(a_map_id,a_player_thing_id);
  END IF;
  IF (v_plan & 16) <> 0 THEN
    DO doom_cs_monster_attack(a_map_id,a_player_thing_id);
  END IF;
  IF (v_plan & 32) <> 0 THEN
    DO doom_cs_monster_barrel(a_map_id);
  END IF;
  IF (v_plan & 64) <> 0 THEN
    DO doom_cs_monster_blast(a_map_id);
  END IF;
  SET v_respawning = doom_cs_monster_deaths(a_map_id,a_player_thing_id);
  IF v_respawning THEN
    DO doom_cs_monster_respawn(a_map_id,a_player_thing_id);
  END IF;
  RETURN 0;
END;;

-- ==== 27_cs_item_respawn.sql ====
-- P_RespawnSpecials, deathmatch 2.0 (-altdeath): a special that was picked
-- up comes back 30 seconds later where it stood, with the item fog and its
-- sound.
DROP FUNCTION IF EXISTS doom_cs_item_respawn;;
CREATE FUNCTION doom_cs_item_respawn(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_alt BOOLEAN;
  DECLARE v_item_respawn_tics DOUBLE;
  DECLARE v_dropped_base DOUBLE;
  DECLARE v_fog_base DOUBLE;
  DECLARE v_tic_span DOUBLE;
  DECLARE v_a BOOLEAN;
  DECLARE v_due BIGINT;
  DECLARE v_t BIGINT;
  SET v_alt = false;
  SET v_item_respawn_tics = doom_const('ITEM_RESPAWN_TICS');
  SET v_dropped_base = doom_const('DROPPED_THING_ID_BASE');
  SET v_fog_base = doom_const('ITEM_FOG_EFFECT_ID_BASE');
  SET v_tic_span = doom_const('EFFECT_ID_TIC_SPAN');
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT m.altdeath AS a FROM mp_match m WHERE m.map_id = a_map_id AND m.state = 'playing';
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_a;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_alt = v_a;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_alt THEN
    -- Everything taken and not yet queued: the pickup stage inserted the row this
    -- tic, so the 30 seconds start now.
    INSERT INTO item_respawns (map_id, thing_id, respawn_tic)
    SELECT pu.map_id, pu.thing_id, (ps.level_tics + v_item_respawn_tics)::bigint
    FROM picked_up_items pu
    JOIN things t ON t.map_id = pu.map_id AND t.id = pu.thing_id
    JOIN player_state ps ON ps.map_id = pu.map_id AND ps.player_thing_id = a_player_thing_id
    -- A monster's drop is gone for good; only a real map item comes back.
    WHERE pu.map_id = a_map_id AND pu.thing_id < v_dropped_base
      AND EXISTS (SELECT 1 FROM pickup_defs d WHERE d.thing_type = t.type)
      AND NOT EXISTS (SELECT 1 FROM item_respawns r WHERE r.map_id = pu.map_id AND r.thing_id = pu.thing_id);
    SET v_due = -1;
    BEGIN
      DECLARE b2_done INT DEFAULT 0;
      DECLARE b2_rows CURSOR FOR SELECT r.thing_id AS t
        FROM item_respawns r
        JOIN player_state ps ON ps.map_id = r.map_id AND ps.player_thing_id = a_player_thing_id
        WHERE r.map_id = a_map_id AND ps.level_tics >= r.respawn_tic
        ORDER BY r.respawn_tic, r.thing_id LIMIT 1;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
      OPEN b2_rows;
      b2_each: LOOP
        FETCH b2_rows INTO v_t;
        IF b2_done THEN LEAVE b2_each; END IF;
        SET v_due = v_t;
      END LOOP;
      CLOSE b2_rows;
    END;
    IF v_due >= 0 THEN
      -- The item fog (IFOG) at the spot, and DSITMBK from it.
      INSERT INTO world_effects (map_id, effect_id, effect_type, x, y, z, sector_id, age)
        SELECT t.map_id, v_fog_base::bigint + t.id::bigint * v_tic_span
                         + (ps.level_tics % v_tic_span),
               'ifog', t.x, t.y, s.floor_height, rt.sector_id, 0
        FROM things t
        JOIN render_things rt ON rt.map_id = t.map_id AND rt.thing_id = t.id
        JOIN sectors s ON s.map_id = rt.map_id AND s.id = rt.sector_id
        JOIN player_state ps ON ps.map_id = t.map_id AND ps.player_thing_id = a_player_thing_id
        WHERE t.map_id = a_map_id AND t.id = v_due
        ON CONFLICT (map_id, effect_id) DO NOTHING;
      INSERT INTO sound_events (map_id, event_key, level_tic, sound_name, source_thing_id, source_x, source_y)
        SELECT t.map_id, 'itemrespawn:' || t.id::text || ':' || ps.level_tics::text, ps.level_tics,
               'DSITMBK', t.id, t.x, t.y
        FROM things t
        JOIN player_state ps ON ps.map_id = t.map_id AND ps.player_thing_id = a_player_thing_id
        WHERE t.map_id = a_map_id AND t.id = v_due
        ON CONFLICT (map_id, event_key) DO NOTHING;
      DELETE FROM picked_up_items WHERE map_id = a_map_id AND thing_id = v_due;
      DELETE FROM item_respawns WHERE map_id = a_map_id AND thing_id = v_due;
    END IF;
  END IF;
  RETURN 1;
END;;

-- ==== 28_cs_sector_fx.sql ====
DROP FUNCTION IF EXISTS doom_cs_sector_fx;;
CREATE FUNCTION doom_cs_sector_fx(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_damage_interval DOUBLE;
  SET v_damage_interval = doom_const('DAMAGE_FLOOR_INTERVAL');
  -- Sector specials: damaging floors.
  UPDATE player_state ps
  SET god_mode=FALSE
  FROM sectors s
  JOIN sector_special_defs sd ON sd.special=s.special AND sd.ends_level_at_low_health
  WHERE s.map_id=ps.map_id AND s.id=ps.sector_id
    AND ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
    AND ps.god_mode;
  -- Damaging floors. Doom applies these once every 32 tics while the player
  -- stands in the sector: nukage 5, slime 10, and the harsher 20% variants.
  -- A radiation suit blocks them outright here; vanilla still lets the 20%
  -- floors through one time in sixteen, which is not modelled.
  UPDATE player_state ps
  SET armor = ps.armor - d.saved,
      armor_class = CASE WHEN ps.armor - d.saved <= 0 THEN 0
                         ELSE ps.armor_class END,
      health = GREATEST(0, ps.health - (d.amount - d.saved)),
      -- P_DamageMobj: player->damagecount += damage, which reddens the screen.
      damage_count = LEAST(100, ps.damage_count + (d.amount - d.saved)),
      pain_face_tics = 12,
      alive = (ps.health - (d.amount - d.saved)) > 0,
      -- The floor is the world, for the deathmatch scoreboard.
      killer_id = CASE WHEN (ps.health - (d.amount - d.saved)) <= 0 THEN -1
                       ELSE ps.killer_id END
  FROM (
    SELECT s.map_id, s.id AS sector_id, ps2.player_thing_id, k.amount,
           LEAST(ps2.armor, CASE ps2.armor_class
                              WHEN 2 THEN FLOOR(k.amount/2.0)::int
                              WHEN 1 THEN FLOOR(k.amount/3.0)::int
                              ELSE 0 END) AS saved
    FROM sectors s
    JOIN sector_special_defs sd ON sd.special=s.special
     AND sd.damage_per_hit IS NOT NULL
    CROSS JOIN LATERAL (
      SELECT sd.damage_per_hit
             >> CASE WHEN (SELECT g.skill FROM game_tic_commands g
                       WHERE g.map_id=a_map_id::int
                         AND g.player_thing_id=a_player_thing_id::int)=0 THEN 1 ELSE 0 END AS amount
    ) k
    JOIN player_state ps2 ON ps2.map_id = s.map_id
     AND ps2.player_thing_id = a_player_thing_id::int
    WHERE s.map_id=a_map_id
  ) d
  WHERE ps.map_id=d.map_id AND ps.player_thing_id=d.player_thing_id
    AND ps.sector_id=d.sector_id
    AND ps.alive AND ps.radsuit_tics<=0
    -- God mode and invulnerability stop damage at the source.
    AND NOT ps.god_mode AND ps.invuln_tics <= 0
    AND (ps.level_tics % v_damage_interval)=0;
  RETURN 0;
END;;

-- ==== 29_cs_thing_physics.sql ====
DROP FUNCTION IF EXISTS doom_cs_thing_physics;;
CREATE FUNCTION doom_cs_thing_physics(a_map_id INT) RETURNS INT
BEGIN
  DECLARE v_moved BIGINT;
  DECLARE v_friction DOUBLE;
  DECLARE v_stop_speed DOUBLE;
  DECLARE v_max_step DOUBLE;
  DECLARE v_any_moving BOOLEAN;
  DECLARE v_e BOOLEAN;
  -- P_XYMovement for Things.
  --
  -- Doom carries every mobj by its momentum each tic and multiplies it by
  -- FRICTION (0xe800/65536 = 0.90625) once it is on the ground (nothing slows a
  -- thing that is in the air, z > floorz: a shoved cacodemon drifts); a step that runs
  -- into something stops dead rather than sliding along it, because P_TryMove
  -- refuses the move and P_XYMovement zeroes the momentum for anything that is
  -- not a missile or a skull. STOPSPEED (0x1000/65536 = 0.0625) is where it
  -- gives up and calls the thing still.
  --
  -- Only things with momentum are touched, so an ordinary tic reads a handful of
  -- rows and writes none: the shove from P_DamageMobj is the only thing that
  -- ever sets it, and it dies away within a couple of seconds.
  SET v_moved = 0;
  SET v_friction = doom_const('FRICTION');
  SET v_stop_speed = doom_const('STOPSPEED');
  SET v_max_step = doom_const('MAXSTEP');
  SET v_any_moving = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS (
        SELECT 1 FROM things
        WHERE map_id = a_map_id AND (ABS(mom_x) > v_stop_speed OR ABS(mom_y) > v_stop_speed)) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_any_moving = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_any_moving THEN
    WITH
    moving AS (
      SELECT t.id, t.x::float8 AS x, t.y::float8 AS y,
             t.mom_x::float8 AS mx, t.mom_y::float8 AS my,
             cd.radius::float8 AS radius,
             -- MF_SKULLFLY: steps do not stop it and it keeps its speed until
             -- it hits something
             cd.skull_fly AS skull,
             cd.floats AND COALESCE(h.alive, FALSE) AND t.z > sec.floor_height AS airborne
      FROM things t
      JOIN thing_combat_defs cd ON cd.thing_type = t.type
      JOIN render_things rt ON rt.map_id = t.map_id AND rt.thing_id = t.id
      LEFT JOIN thing_health h ON h.map_id = t.map_id AND h.thing_id = t.id
      LEFT JOIN monster_ai ai ON ai.map_id = t.map_id AND ai.thing_id = t.id
      JOIN sectors sec ON sec.map_id = t.map_id AND sec.id = COALESCE(ai.sector_id, rt.sector_id)
      WHERE t.map_id = a_map_id
        AND (ABS(t.mom_x) > v_stop_speed OR ABS(t.mom_y) > v_stop_speed)
        -- Players move through doom_cs_move; their Thing only mirrors it.
        AND NOT EXISTS (SELECT 1 FROM player_state ps
                        WHERE ps.map_id = t.map_id AND ps.player_thing_id = t.id)
    ),
    -- P_TryMove's line check: the step is refused if it crosses a one-sided line,
    -- an explicitly blocking one, or a step too tall to climb. Bounded by the two
    -- bounding boxes overlapping, the same way the monster mover bounds its own.
    wall_hit AS (
      SELECT m.id, TRUE AS hit
      FROM moving m
      JOIN linedef_geom ld ON ld.map_id = a_map_id
      LEFT JOIN sectors fr ON fr.map_id = ld.map_id AND fr.id = ld.fsec
      LEFT JOIN sectors bk ON bk.map_id = ld.map_id AND bk.id = ld.bsec
      WHERE (ld.left_sd_id = -1 OR ld.right_sd_id = -1 OR (ld.flags & 1) <> 0
          OR (NOT m.skull AND fr.floor_height IS NOT NULL AND bk.floor_height IS NOT NULL
              AND ABS(fr.floor_height - bk.floor_height) > v_max_step))
        AND LEAST(ld.x1,ld.x2) <= GREATEST(m.x, m.x+m.mx) + m.radius
        AND GREATEST(ld.x1,ld.x2) >= LEAST(m.x, m.x+m.mx) - m.radius
        AND LEAST(ld.y1,ld.y2) <= GREATEST(m.y, m.y+m.my) + m.radius
        AND GREATEST(ld.y1,ld.y2) >= LEAST(m.y, m.y+m.my) - m.radius
        AND ((m.x+m.mx - ld.x1)*(ld.y2-ld.y1) - (m.y+m.my - ld.y1)*(ld.x2-ld.x1))
          * ((m.x - ld.x1)*(ld.y2-ld.y1) - (m.y - ld.y1)*(ld.x2-ld.x1)) < 0
        AND ((ld.x1 - m.x)*m.my - (ld.y1 - m.y)*m.mx)
          * ((ld.x2 - m.x)*m.my - (ld.y2 - m.y)*m.mx) < 0
      GROUP BY m.id
    ),
    -- PIT_CheckThing: and it stops on anything solid standing where it is going.
    thing_hit AS (
      SELECT m.id, TRUE AS hit
      FROM moving m
      JOIN things o ON o.map_id = a_map_id AND o.id <> m.id
      JOIN thing_combat_defs od ON od.thing_type = o.type
      JOIN thing_health oh ON oh.map_id = o.map_id AND oh.thing_id = o.id
       AND oh.alive
      WHERE ABS(o.x - (m.x+m.mx)) < od.radius + m.radius
        AND ABS(o.y - (m.y+m.my)) < od.radius + m.radius
      GROUP BY m.id
    ),
    step AS (
      SELECT m.*, COALESCE(w.hit,FALSE) OR COALESCE(th.hit,FALSE) AS blocked
      FROM moving m
      LEFT JOIN wall_hit w ON w.id = m.id
      LEFT JOIN thing_hit th ON th.id = m.id
    )
    -- The geometry above runs in float8; the write-back adds and multiplies the
    -- real columns directly, so the row body carries no casts or range checks.
    UPDATE things t
    SET x = CASE WHEN s.blocked THEN t.x ELSE t.x + t.mom_x END,
        y = CASE WHEN s.blocked THEN t.y ELSE t.y + t.mom_y END,
        -- FRICTION once it has moved; a blocked step loses its momentum outright,
        -- a skull or an airborne thing keeps it.
        mom_x = CASE WHEN s.blocked THEN 0 WHEN s.skull OR s.airborne THEN t.mom_x ELSE t.mom_x * v_friction END,
        mom_y = CASE WHEN s.blocked THEN 0 WHEN s.skull OR s.airborne THEN t.mom_y ELSE t.mom_y * v_friction END
    FROM step s
    WHERE t.map_id = a_map_id AND t.id = s.id;
  END IF;
  -- Anything below STOPSPEED is standing still; zero it so the statement above
  -- stops finding it.
  UPDATE things
  SET mom_x = 0, mom_y = 0
  WHERE map_id = a_map_id
    AND ABS(mom_x) <= v_stop_speed AND ABS(mom_y) <= v_stop_speed
    AND (mom_x <> 0 OR mom_y <> 0);
  RETURN v_moved;
END;;

-- ==== 30_carry_player.sql ====
DROP FUNCTION IF EXISTS doom_carry_player;;
CREATE FUNCTION doom_carry_player(a_from_map_id INT, a_from_player_thing_id INT, a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Carry the player across a level exit, the way vanilla does.
  --
  -- G_PlayerFinishLevel keeps weapons, ammo, armor, backpack and health -- it
  -- does NOT top health up  and clears only powerups, keys and the screen effects.
  UPDATE player_state ps
  SET health=src.health,
      armor=src.armor, armor_class=src.armor_class, backpack=src.backpack,
      ammo_bullets=src.ammo_bullets, ammo_shells=src.ammo_shells,
      ammo_rockets=src.ammo_rockets, ammo_cells=src.ammo_cells
  FROM player_state src
  WHERE ps.map_id=a_map_id::int AND ps.player_thing_id=a_player_thing_id::int
    AND src.map_id=a_from_map_id::int
    AND src.player_thing_id=a_from_player_thing_id::int
    -- A dead player gets the reborn loadout, so carry nothing.
    AND src.alive AND src.health>0;
  DELETE FROM player_weapon_owned o
  WHERE o.map_id=a_map_id::int AND o.player_thing_id=a_player_thing_id::int
    AND EXISTS (SELECT 1 FROM player_state src
                WHERE src.map_id=a_from_map_id::int
                  AND src.player_thing_id=a_from_player_thing_id::int
                  AND src.alive AND src.health>0);
  INSERT INTO player_weapon_owned (map_id, player_thing_id, weapon_id)
  SELECT a_map_id::int, a_player_thing_id::int, o.weapon_id
  FROM player_weapon_owned o
  JOIN player_state src
    ON src.map_id=o.map_id AND src.player_thing_id=o.player_thing_id
  WHERE o.map_id=a_from_map_id::int
    AND o.player_thing_id=a_from_player_thing_id::int
    AND src.alive AND src.health>0
  ON CONFLICT (map_id,player_thing_id,weapon_id) DO NOTHING;
  -- Enter holding what you left with. The row keeps reset_stage's state='up'
  -- and sy=128, so the weapon plays its raise animation on entry as it should.
  UPDATE player_weapons pw
  SET current_weapon=srcw.current_weapon
  FROM player_weapons srcw
  JOIN player_state src
    ON src.map_id=srcw.map_id AND src.player_thing_id=srcw.player_thing_id
  WHERE pw.map_id=a_map_id::int AND pw.player_thing_id=a_player_thing_id::int
    AND srcw.map_id=a_from_map_id::int
    AND srcw.player_thing_id=a_from_player_thing_id::int
    AND src.alive AND src.health>0;
  RETURN 0;
END;;

-- ==== 31_menu.sql ====
DROP FUNCTION IF EXISTS doom_menu_input;;
CREATE FUNCTION doom_menu_input(a_key TEXT) RETURNS TEXT
BEGIN
  DECLARE v_action TEXT;
  DECLARE v_act BOOLEAN;
  -- One keypress against the title/menu state machine
  SET v_action = 'none';
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT
        CASE
          WHEN s.screen='skill'  AND a_key='enter' THEN 'start'
          WHEN s.screen='main'   AND a_key='enter' AND s.cursor_index=5 THEN 'quit'
          WHEN s.screen='intermission' AND a_key IN ('enter','escape') THEN 'advance'
          WHEN s.screen='load' AND a_key='enter' AND EXISTS (
            SELECT 1 FROM save_slots v WHERE v.slot=s.cursor_index)
            THEN 'load:' || s.cursor_index::text
          WHEN s.screen='save' AND a_key='enter'
            THEN 'save:' || s.cursor_index::text
          ELSE 'none'
        END AS act
      FROM screen_state s WHERE s.id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_act;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_action = v_act;
    END LOOP;
    CLOSE b1_rows;
  END;
  UPDATE screen_state s
  SET screen = CASE
        WHEN s.screen='title' THEN 'main'::screen_kind
        WHEN s.screen='main' AND a_key='enter' AND s.cursor_index=0 THEN 'episode'::screen_kind
        WHEN s.screen='main' AND a_key='enter' AND s.cursor_index=4 THEN 'help1'::screen_kind
        WHEN s.screen='help1' AND a_key='enter' THEN 'help2'::screen_kind
        WHEN s.screen='help2' AND a_key='enter' THEN 'main'::screen_kind
        WHEN s.screen IN ('help1','help2') AND a_key='escape' THEN 'main'::screen_kind
        WHEN s.screen='main' AND a_key='enter' AND s.cursor_index=2 THEN 'load'::screen_kind
        WHEN s.screen='main' AND a_key='enter' AND s.cursor_index=3 THEN 'save'::screen_kind
        WHEN s.screen IN ('load','save') AND a_key='escape' THEN 'main'::screen_kind
        WHEN s.screen='main' AND a_key='escape' THEN 'game'::screen_kind
        WHEN s.screen='save' AND a_key='enter' THEN 'game'::screen_kind
        WHEN s.screen='load' AND a_key='enter' AND EXISTS (
          SELECT 1 FROM save_slots v WHERE v.slot=s.cursor_index) THEN 'game'::screen_kind
        WHEN s.screen='episode' AND a_key='enter' THEN 'skill'::screen_kind
        WHEN s.screen='episode' AND a_key='escape' THEN 'main'::screen_kind
        WHEN s.screen='skill' AND a_key='escape' THEN 'episode'::screen_kind
        WHEN s.screen='skill' AND a_key='enter' THEN 'game'::screen_kind
        WHEN s.screen='intermission' AND a_key IN ('enter','escape') THEN 'game'::screen_kind
        ELSE s.screen
      END,
      cursor_index = CASE
        WHEN s.screen='title' THEN 0
        WHEN a_key='enter' AND s.screen='episode' THEN 2
        WHEN a_key='enter' AND ((s.screen='main' AND s.cursor_index IN (0,2,3))
                                OR s.screen IN ('skill','intermission',
                                                'load','save'))
          THEN 0
        WHEN s.screen IN ('help1','help2') THEN 4
        WHEN a_key='escape' AND s.screen='skill' THEN 0
        WHEN a_key='escape' AND s.screen IN ('episode','intermission',
                                             'load','save','main')
          THEN 0
        WHEN a_key='down' THEN (s.cursor_index + 1)
          % GREATEST(1,(SELECT count(*)::int FROM menu_items m WHERE m.screen=s.screen))
        WHEN a_key='up' THEN (s.cursor_index - 1
          + GREATEST(1,(SELECT count(*)::int FROM menu_items m WHERE m.screen=s.screen)))
          % GREATEST(1,(SELECT count(*)::int FROM menu_items m WHERE m.screen=s.screen))
        ELSE s.cursor_index
      END,
      episode = CASE WHEN s.screen='episode' AND a_key='enter'
                     THEN s.cursor_index + 1 ELSE s.episode END,
      -- Doom's skill runs 0..4; the tic pipeline wants the Thing option bit,
      -- which the client derives from this.
      skill = CASE WHEN s.screen='skill' AND a_key='enter'
                   THEN s.cursor_index ELSE s.skill END
  WHERE s.id=0;
  RETURN v_action;
END;;

-- ==== 32_save_load.sql ====
DROP FUNCTION IF EXISTS doom_save_game;;
CREATE FUNCTION doom_save_game(a_slot INT, a_map_id INT, a_player_thing_id INT, a_skill INT, a_name TEXT) RETURNS INT
BEGIN
  DELETE FROM save_slots WHERE slot=a_slot::int;
  INSERT INTO save_slots (slot,saved_at,name,map_id,player_thing_id,map_name,skill)
  SELECT a_slot::int, NOW(), a_name::text, a_map_id::int,
         a_player_thing_id::int,
         (SELECT m.name FROM maps m WHERE m.map_id=a_map_id::int),
         -- The skill the WORLD was built with, taken from
         -- level_stats rather than the caller. reset_stage seeds
         -- thing_health/monster_ai per skill, so a save that
         -- recorded a different skill than its own world loaded
         -- with monsters that had no health and no AI row: they
         -- rendered alive, never attacked, and cost frame time.
         COALESCE((SELECT ls.skill_bit FROM level_stats ls
                    WHERE ls.map_id=a_map_id::int
                      AND ls.player_thing_id=a_player_thing_id::int),
                  a_skill::int);
  DELETE FROM save_player_state WHERE slot=a_slot::int;
  INSERT INTO save_player_state (slot,map_id,player_thing_id,health,alive,level_tics,previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle,momentum_x,momentum_y,bob_strength,previous_view_z,previous_view_angle,sector_id,pain_face_tics,armor,armor_class,backpack,ammo_bullets,ammo_shells,ammo_rockets,ammo_cells,key_blue,key_yellow,key_red,radsuit_tics,invis_tics,momentum_z,damage_count,bonus_count,light_amp_tics,power_map,god_mode,noclip,invuln_tics,berserk,message,message_tics,frags,death_tics,killer_id,sprite_frame)
  SELECT a_slot::int,map_id,player_thing_id,health,alive,level_tics,previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle,momentum_x,momentum_y,bob_strength,previous_view_z,previous_view_angle,sector_id,pain_face_tics,armor,armor_class,backpack,ammo_bullets,ammo_shells,ammo_rockets,ammo_cells,key_blue,key_yellow,key_red,radsuit_tics,invis_tics,momentum_z,damage_count,bonus_count,light_amp_tics,power_map,god_mode,noclip,invuln_tics,berserk,message,message_tics,frags,death_tics,killer_id,sprite_frame FROM player_state WHERE map_id=a_map_id::int;
  DELETE FROM save_player_weapons WHERE slot=a_slot::int;
  INSERT INTO save_player_weapons (slot,map_id,player_thing_id,current_weapon,pending_weapon,state,seq_index,tics,flash_seq_index,flash_tics,sx,sy,shot_serial,fired_this_tick)
  SELECT a_slot::int,map_id,player_thing_id,current_weapon,pending_weapon,state,seq_index,tics,flash_seq_index,flash_tics,sx,sy,shot_serial,fired_this_tick FROM player_weapons WHERE map_id=a_map_id::int;
  DELETE FROM save_player_weapon_owned WHERE slot=a_slot::int;
  INSERT INTO save_player_weapon_owned (slot,map_id,player_thing_id,weapon_id)
  SELECT a_slot::int,map_id,player_thing_id,weapon_id FROM player_weapon_owned WHERE map_id=a_map_id::int;
  DELETE FROM save_things WHERE slot=a_slot::int;
  INSERT INTO save_things (slot,id,map_id,x,y,z,angle,spawn_x,spawn_y,spawn_angle,mom_x,mom_y,type,flags)
  SELECT a_slot::int,id,map_id,x,y,z,angle,spawn_x,spawn_y,spawn_angle,mom_x,mom_y,type,flags FROM things WHERE map_id=a_map_id::int;
  DELETE FROM save_thing_health WHERE slot=a_slot::int;
  INSERT INTO save_thing_health (slot,map_id,thing_id,health,max_health,alive)
  SELECT a_slot::int,map_id,thing_id,health,max_health,alive FROM thing_health WHERE map_id=a_map_id::int;
  DELETE FROM save_monster_ai WHERE slot=a_slot::int;
  INSERT INTO save_monster_ai (slot,map_id,thing_id,state,state_tics,seq_index,sector_id,attack_cooldown,fired_this_tick,target_thing_id,charge_tics,movedir,movecount)
  SELECT a_slot::int,map_id,thing_id,state,state_tics,seq_index,sector_id,attack_cooldown,fired_this_tick,target_thing_id,charge_tics,movedir,movecount FROM monster_ai WHERE map_id=a_map_id::int;
  DELETE FROM save_monster_projectiles WHERE slot=a_slot::int;
  INSERT INTO save_monster_projectiles (slot,map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,sector_id,state,age,damage,impact_player)
  SELECT a_slot::int,map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,sector_id,state,age,damage,impact_player FROM monster_projectiles WHERE map_id=a_map_id::int;
  DELETE FROM save_world_effects WHERE slot=a_slot::int;
  INSERT INTO save_world_effects (slot,map_id,effect_id,effect_type,x,y,z,sector_id,age)
  SELECT a_slot::int,map_id,effect_id,effect_type,x,y,z,sector_id,age FROM world_effects WHERE map_id=a_map_id::int;
  DELETE FROM save_sectors WHERE slot=a_slot::int;
  INSERT INTO save_sectors (slot,id,map_id,floor_height,ceil_height,spawn_floor_height,spawn_ceil_height,spawn_floor_tex,spawn_light_level,floor_tex,ceil_tex,light_level,special,tag)
  SELECT a_slot::int,id,map_id,floor_height,ceil_height,spawn_floor_height,spawn_ceil_height,spawn_floor_tex,spawn_light_level,floor_tex,ceil_tex,light_level,special,tag FROM sectors WHERE map_id=a_map_id::int;
  DELETE FROM save_sidedefs WHERE slot=a_slot::int;
  INSERT INTO save_sidedefs (slot,id,map_id,x_offset,y_offset,upper_tex,lower_tex,mid_tex,sector_id)
  SELECT a_slot::int,id,map_id,x_offset,y_offset,upper_tex,lower_tex,mid_tex,sector_id FROM sidedefs WHERE map_id=a_map_id::int;
  DELETE FROM save_sector_movers WHERE slot=a_slot::int;
  INSERT INTO save_sector_movers (slot,map_id,sector_id,source_line_id,mover_type,plane,direction,bottom_height,top_height,speed,move_carry,wait_tics,countdown,next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush)
  SELECT a_slot::int,map_id,sector_id,source_line_id,mover_type,plane,direction,bottom_height,top_height,speed,move_carry,wait_tics,countdown,next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush FROM sector_movers WHERE map_id=a_map_id::int;
  DELETE FROM save_line_buttons WHERE slot=a_slot::int;
  INSERT INTO save_line_buttons (slot,map_id,line_id,sidedef_id,texture_part,original_tex,active_tex,countdown,just_restored)
  SELECT a_slot::int,map_id,line_id,sidedef_id,texture_part,original_tex,active_tex,countdown,just_restored FROM line_buttons WHERE map_id=a_map_id::int;
  DELETE FROM save_line_activations WHERE slot=a_slot::int;
  INSERT INTO save_line_activations (slot,map_id,line_id)
  SELECT a_slot::int,map_id,line_id FROM line_activations WHERE map_id=a_map_id::int;
  DELETE FROM save_picked_up_items WHERE slot=a_slot::int;
  INSERT INTO save_picked_up_items (slot,map_id,thing_id)
  SELECT a_slot::int,map_id,thing_id FROM picked_up_items WHERE map_id=a_map_id::int;
  DELETE FROM save_level_secret_discoveries WHERE slot=a_slot::int;
  INSERT INTO save_level_secret_discoveries (slot,map_id,player_thing_id,sector_id)
  SELECT a_slot::int,map_id,player_thing_id,sector_id FROM level_secret_discoveries WHERE map_id=a_map_id::int;
  DELETE FROM save_level_stats WHERE slot=a_slot::int;
  INSERT INTO save_level_stats (slot,map_id,player_thing_id,skill_bit,level_tics,par_tics,kills,total_kills,items,total_items,secrets,total_secrets,completed,secret_exit)
  SELECT a_slot::int,map_id,player_thing_id,skill_bit,level_tics,par_tics,kills,total_kills,items,total_items,secrets,total_secrets,completed,secret_exit FROM level_stats WHERE map_id=a_map_id::int;
  RETURN a_slot::int;
END;;

DROP FUNCTION IF EXISTS doom_load_game;;
CREATE FUNCTION doom_load_game(a_slot INT) RETURNS INT
BEGIN
  DECLARE v_target BIGINT;
  DECLARE v_m BIGINT;
  -- Returns the restored map_id, or -1 when the slot is empty.
  SET v_target = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(MIN(map_id),-1) AS m FROM save_slots WHERE slot=a_slot::int;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_m;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_target = v_m;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_target >= 0 THEN
    DELETE FROM player_state WHERE map_id=v_target;
    INSERT INTO player_state (map_id,player_thing_id,health,alive,level_tics,previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle,momentum_x,momentum_y,bob_strength,previous_view_z,previous_view_angle,sector_id,pain_face_tics,armor,armor_class,backpack,ammo_bullets,ammo_shells,ammo_rockets,ammo_cells,key_blue,key_yellow,key_red,radsuit_tics,invis_tics,momentum_z,damage_count,bonus_count,light_amp_tics,power_map,god_mode,noclip,invuln_tics,berserk,message,message_tics,frags,death_tics,killer_id,sprite_frame)
      SELECT map_id,player_thing_id,health,alive,level_tics,previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle,momentum_x,momentum_y,bob_strength,previous_view_z,previous_view_angle,sector_id,pain_face_tics,armor,armor_class,backpack,ammo_bullets,ammo_shells,ammo_rockets,ammo_cells,key_blue,key_yellow,key_red,radsuit_tics,invis_tics,momentum_z,damage_count,bonus_count,light_amp_tics,power_map,god_mode,noclip,invuln_tics,berserk,message,message_tics,frags,death_tics,killer_id,sprite_frame FROM save_player_state WHERE slot=a_slot::int;
    DELETE FROM player_weapons WHERE map_id=v_target;
    INSERT INTO player_weapons (map_id,player_thing_id,current_weapon,pending_weapon,state,seq_index,tics,flash_seq_index,flash_tics,sx,sy,shot_serial,fired_this_tick)
      SELECT map_id,player_thing_id,current_weapon,pending_weapon,state,seq_index,tics,flash_seq_index,flash_tics,sx,sy,shot_serial,fired_this_tick FROM save_player_weapons WHERE slot=a_slot::int;
    DELETE FROM player_weapon_owned WHERE map_id=v_target;
    INSERT INTO player_weapon_owned (map_id,player_thing_id,weapon_id)
      SELECT map_id,player_thing_id,weapon_id FROM save_player_weapon_owned WHERE slot=a_slot::int;
    DELETE FROM things WHERE map_id=v_target;
    INSERT INTO things (id,map_id,x,y,z,angle,spawn_x,spawn_y,spawn_angle,mom_x,mom_y,type,flags)
      SELECT id,map_id,x,y,z,angle,spawn_x,spawn_y,spawn_angle,mom_x,mom_y,type,flags FROM save_things WHERE slot=a_slot::int;
    DELETE FROM thing_health WHERE map_id=v_target;
    INSERT INTO thing_health (map_id,thing_id,health,max_health,alive)
      SELECT map_id,thing_id,health,max_health,alive FROM save_thing_health WHERE slot=a_slot::int;
    DELETE FROM monster_ai WHERE map_id=v_target;
    INSERT INTO monster_ai (map_id,thing_id,state,state_tics,seq_index,sector_id,attack_cooldown,fired_this_tick,target_thing_id,charge_tics,movedir,movecount)
      SELECT map_id,thing_id,state,state_tics,seq_index,sector_id,attack_cooldown,fired_this_tick,target_thing_id,charge_tics,movedir,movecount FROM save_monster_ai WHERE slot=a_slot::int;
    DELETE FROM monster_projectiles WHERE map_id=v_target;
    INSERT INTO monster_projectiles (map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,sector_id,state,age,damage,impact_player)
      SELECT map_id,projectile_id,owner_thing_id,projectile_type,x,y,z,vx,vy,vz,sector_id,state,age,damage,impact_player FROM save_monster_projectiles WHERE slot=a_slot::int;
    DELETE FROM world_effects WHERE map_id=v_target;
    INSERT INTO world_effects (map_id,effect_id,effect_type,x,y,z,sector_id,age)
      SELECT map_id,effect_id,effect_type,x,y,z,sector_id,age FROM save_world_effects WHERE slot=a_slot::int;
    -- The one place this function cannot keep its delete/insert pairs together:
    -- sidedefs reference sectors, so the children go first and come back last.
    DELETE FROM sidedefs WHERE map_id=v_target;
    DELETE FROM sectors WHERE map_id=v_target;
    INSERT INTO sectors (id,map_id,floor_height,ceil_height,spawn_floor_height,spawn_ceil_height,spawn_floor_tex,spawn_light_level,floor_tex,ceil_tex,light_level,special,tag)
      SELECT id,map_id,floor_height,ceil_height,spawn_floor_height,spawn_ceil_height,spawn_floor_tex,spawn_light_level,floor_tex,ceil_tex,light_level,special,tag FROM save_sectors WHERE slot=a_slot::int;
    INSERT INTO sidedefs (id,map_id,x_offset,y_offset,upper_tex,lower_tex,mid_tex,sector_id)
      SELECT id,map_id,x_offset,y_offset,upper_tex,lower_tex,mid_tex,sector_id FROM save_sidedefs WHERE slot=a_slot::int;
    DELETE FROM sector_movers WHERE map_id=v_target;
    INSERT INTO sector_movers (map_id,sector_id,source_line_id,mover_type,plane,direction,bottom_height,top_height,speed,move_carry,wait_tics,countdown,next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush)
      SELECT map_id,sector_id,source_line_id,mover_type,plane,direction,bottom_height,top_height,speed,move_carry,wait_tics,countdown,next_ceiling,next_floor,target_floor_tex,moved_this_tick,crush FROM save_sector_movers WHERE slot=a_slot::int;
    DELETE FROM line_buttons WHERE map_id=v_target;
    INSERT INTO line_buttons (map_id,line_id,sidedef_id,texture_part,original_tex,active_tex,countdown,just_restored)
      SELECT map_id,line_id,sidedef_id,texture_part,original_tex,active_tex,countdown,just_restored FROM save_line_buttons WHERE slot=a_slot::int;
    DELETE FROM line_activations WHERE map_id=v_target;
    INSERT INTO line_activations (map_id,line_id)
      SELECT map_id,line_id FROM save_line_activations WHERE slot=a_slot::int;
    DELETE FROM picked_up_items WHERE map_id=v_target;
    INSERT INTO picked_up_items (map_id,thing_id)
      SELECT map_id,thing_id FROM save_picked_up_items WHERE slot=a_slot::int;
    DELETE FROM level_secret_discoveries WHERE map_id=v_target;
    INSERT INTO level_secret_discoveries (map_id,player_thing_id,sector_id)
      SELECT map_id,player_thing_id,sector_id FROM save_level_secret_discoveries WHERE slot=a_slot::int;
    DELETE FROM level_stats WHERE map_id=v_target;
    INSERT INTO level_stats (map_id,player_thing_id,skill_bit,level_tics,par_tics,kills,total_kills,items,total_items,secrets,total_secrets,completed,secret_exit)
      SELECT map_id,player_thing_id,skill_bit,level_tics,par_tics,kills,total_kills,items,total_items,secrets,total_secrets,completed,secret_exit FROM save_level_stats WHERE slot=a_slot::int;
    -- Nothing half-finished from the old session may survive:
    -- these are rebuilt every tic and a stale row would fire.
    DELETE FROM game_tic_commands WHERE map_id=v_target;
    DELETE FROM line_special_events WHERE map_id=v_target;
    DELETE FROM line_use_results WHERE map_id=v_target;
    DELETE FROM pickup_touches WHERE map_id=v_target;
    DELETE FROM pickup_grants WHERE map_id=v_target;
    DELETE FROM sound_events WHERE map_id=v_target;
    DELETE FROM hitscan_hits WHERE map_id=v_target;
    DELETE FROM projectile_damage WHERE map_id=v_target;
    DELETE FROM projectile_impacts WHERE map_id=v_target;
    DELETE FROM monster_attack_damage WHERE map_id=v_target;
    -- Derived mirrors, rebuilt from the state just restored.
    DO doom_materialize_render_segs(v_target);
    DO doom_materialize_render_things(v_target);
    DELETE FROM sector_light_fx WHERE map_id=v_target;
    INSERT INTO sector_light_fx
        (map_id, sector_id, special, base_light, dark_light)
      SELECT s.map_id, s.id, s.special::smallint,
             COALESCE(s.spawn_light_level, s.light_level)::smallint,
             LEAST(COALESCE(s.spawn_light_level, s.light_level),
                   COALESCE(MIN(other.light_level), 0::smallint))::smallint
      FROM sectors s
      LEFT JOIN linedefs edge ON edge.map_id = s.map_id
      LEFT JOIN sidedefs r ON r.map_id = edge.map_id AND r.id = edge.right_sd_id
      LEFT JOIN sidedefs l ON l.map_id = edge.map_id AND l.id = edge.left_sd_id
      LEFT JOIN sectors other ON other.map_id = s.map_id
        AND other.id = CASE WHEN r.sector_id = s.id THEN l.sector_id
                            WHEN l.sector_id = s.id THEN r.sector_id END
      WHERE s.map_id = v_target AND s.special IN (1,2,3,8,12,13,17)
      GROUP BY s.map_id, s.id, s.special, s.spawn_light_level, s.light_level;
  END IF;
  RETURN v_target;
END;;

-- ==== 33_intermission.sql ====
DROP FUNCTION IF EXISTS doom_intermission_begin;;
CREATE FUNCTION doom_intermission_begin(a_map_id INT, a_player_thing_id INT, a_secret_exit BOOLEAN) RETURNS INT
BEGIN
  DECLARE v_next_id BIGINT;
  DECLARE v_n BIGINT;
  -- Enter the intermission for a finished level and work out what comes next.
  SET v_next_id = -1;
  WITH cur AS (
    SELECT m.map_id, m.name,
           substring(m.name FROM 2 FOR 1)::int AS ep,
           substring(m.name FROM 4 FOR 1)::int AS lv
    FROM maps m WHERE m.map_id = a_map_id::int
  ),
  route AS (
    SELECT c.*,
      CASE
        WHEN a_secret_exit::boolean THEN 9
        WHEN c.lv = 9 THEN
          CASE c.ep WHEN 1 THEN 4 WHEN 2 THEN 6 WHEN 3 THEN 7 ELSE 3 END
        WHEN c.lv = 8 THEN NULL
        ELSE c.lv + 1
      END AS next_lv
    FROM cur c
  ),
  resolved AS (
    SELECT r.*,
           (SELECT n.map_id FROM maps n
             WHERE n.name = 'E' || r.ep::text || 'M' || r.next_lv::text) AS next_id
    FROM route r
  ),
  stats AS (
    SELECT ls.*,
           -- Doom divides by the level totals; a level holding none of a thing
           -- shows zero rather than dividing by zero.
           CASE WHEN ls.total_kills   > 0
                THEN LEAST(100, ls.kills   * 100 / ls.total_kills)   ELSE 0 END AS pk,
           CASE WHEN ls.total_items   > 0
                THEN LEAST(100, ls.items   * 100 / ls.total_items)   ELSE 0 END AS pi,
           CASE WHEN ls.total_secrets > 0
                THEN LEAST(100, ls.secrets * 100 / ls.total_secrets) ELSE 0 END AS ps
    FROM level_stats ls
    WHERE ls.map_id = a_map_id::int AND ls.player_thing_id = a_player_thing_id::int
  )
  UPDATE screen_state s
  SET screen='intermission', cursor_index=0,
      inter_episode=r.ep, inter_level=r.lv,
      inter_next=COALESCE(r.next_lv, r.lv),
      next_map_id=r.next_id, secret_exit=a_secret_exit::boolean,
      tgt_kills=st.pk, tgt_items=st.pi, tgt_secrets=st.ps,
      tgt_time=(st.level_tics/35)::int,
      tgt_par=COALESCE((st.par_tics/35)::int, 0),
      kills_pct=0, items_pct=0, secrets_pct=0, time_secs=0, par_secs=0,
      sp_state=1, cnt_pause=35, accelerate=FALSE
  FROM resolved r, stats st
  WHERE s.id=0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(MIN(next_map_id), -1) AS n FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_n;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_next_id = v_n;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_next_id;
END;;

DROP FUNCTION IF EXISTS doom_intermission_accelerate;;
CREATE FUNCTION doom_intermission_accelerate() RETURNS INT
BEGIN
  -- A keypress during the tally. Doom's acceleratestage: it snaps the current
  -- figure to its target, and once everything is shown it leaves the screen.
  UPDATE screen_state SET accelerate=TRUE WHERE id=0;
  RETURN 0;
END;;

DROP FUNCTION IF EXISTS doom_intermission_tic;;
CREATE FUNCTION doom_intermission_tic() RETURNS TEXT
BEGIN
  DECLARE v_result TEXT;
  DECLARE v_pre_accel BOOLEAN;
  DECLARE v_pre_state BIGINT;
  DECLARE v_a BOOLEAN;
  DECLARE v_st BIGINT;
  DECLARE v_r BOOLEAN;
  -- One 35 Hz step of WI_updateStats
  --   sp_state 2 kills, 4 items, 6 secrets  -- +2 percent a tic
  --   sp_state 8 time and par               -- +3 seconds a tic
  --   odd states                            -- a one-second pause
  --   sp_state 10                           -- everything shown, waiting
  --
  -- Returns 'counting' while the tally runs, 'waiting' once it is all on
  -- screen, and 'done' when a keypress has dismissed it.
  SET v_result = 'counting';
  SET v_pre_accel = false;
  SET v_pre_state = 0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT accelerate AS a, sp_state AS st FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_a, v_st;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_pre_accel = v_a;
      SET v_pre_state = v_st;
    END LOOP;
    CLOSE b1_rows;
  END;
  UPDATE screen_state s
  SET
    -- An accelerating keypress fills every figure in at once.
    kills_pct = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN s.tgt_kills
        WHEN s.sp_state = 2 THEN LEAST(s.tgt_kills, s.kills_pct + 2)
        ELSE s.kills_pct END,
    items_pct = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN s.tgt_items
        WHEN s.sp_state = 4 THEN LEAST(s.tgt_items, s.items_pct + 2)
        ELSE s.items_pct END,
    secrets_pct = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN s.tgt_secrets
        WHEN s.sp_state = 6 THEN LEAST(s.tgt_secrets, s.secrets_pct + 2)
        ELSE s.secrets_pct END,
    time_secs = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN s.tgt_time
        WHEN s.sp_state = 8 THEN LEAST(s.tgt_time, s.time_secs + 3)
        ELSE s.time_secs END,
    par_secs = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN s.tgt_par
        WHEN s.sp_state = 8 THEN LEAST(s.tgt_par, s.par_secs + 3)
        ELSE s.par_secs END,
    sp_state = CASE
        WHEN s.accelerate AND s.sp_state <> 10 THEN 10
        -- A counting state ends when its figure reaches the target.
        WHEN s.sp_state = 2 AND s.kills_pct + 2 >= s.tgt_kills THEN 3
        WHEN s.sp_state = 4 AND s.items_pct + 2 >= s.tgt_items THEN 5
        WHEN s.sp_state = 6 AND s.secrets_pct + 2 >= s.tgt_secrets THEN 7
        WHEN s.sp_state = 8 AND s.time_secs + 3 >= s.tgt_time
                            AND s.par_secs + 3 >= s.tgt_par THEN 10
        -- A pause state ends when its second runs out.
        WHEN s.sp_state IN (1,3,5,7,9) AND s.cnt_pause <= 1 THEN s.sp_state + 1
        ELSE s.sp_state END,
    cnt_pause = CASE
        WHEN s.sp_state IN (1,3,5,7,9) AND s.cnt_pause > 1 THEN s.cnt_pause - 1
        WHEN s.sp_state IN (1,3,5,7,9) THEN 35
        ELSE s.cnt_pause END,
    accelerate = FALSE
  WHERE s.id = 0;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT CASE WHEN screen <> 'intermission' THEN 'done'
                  WHEN sp_state >= 10 THEN 'waiting'
                  ELSE 'counting' END AS r
      FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_r;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_result = v_r;
    END LOOP;
    CLOSE b2_rows;
  END;
  IF v_pre_accel AND v_pre_state >= 10 THEN
    UPDATE screen_state SET screen='game', sp_state=0, cnt_pause=0 WHERE id=0;
    SET v_result = 'done';
  END IF;
  RETURN v_result;
END;;

-- ==== 34_cs_death.sql ====
DROP FUNCTION IF EXISTS doom_cs_death;;
CREATE FUNCTION doom_cs_death(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- P_DeathThink's view drop, which is the whole of Doom's "death screen".
  --
  -- Doom has no death screen. When you die the view sinks from the normal
  -- 41-unit eye height to 6 units, one unit per tic, so the camera settles on
  -- the floor beside your own corpse
  UPDATE player_state ps
  SET view_z = GREATEST(s.floor_height::float8 + 6.0, ps.view_z - 1.0),
      previous_view_z = ps.view_z, previous_view_angle = ps.view_angle,
      damage_count = GREATEST(0, ps.damage_count - 1),
      death_tics = ps.death_tics + 1,
      momentum_x = 0, momentum_y = 0, momentum_z = 0, bob_strength = 0
  FROM sectors s
  WHERE ps.map_id = a_map_id::int
    AND ps.player_thing_id = a_player_thing_id::int
    AND NOT ps.alive
    AND s.map_id = ps.map_id AND s.id = ps.sector_id;
  RETURN 0;
END;;

-- ==== 35_cs_boss.sql ====
DROP FUNCTION IF EXISTS doom_cs_boss;;
CREATE FUNCTION doom_cs_boss(a_map_id INT) RETURNS INT
BEGIN
  DECLARE v_ready BIGINT;
  DECLARE v_r BOOLEAN;
  SET v_ready = 0;
  -- A map with no boss row, or one whose boss Things were filtered out by the
  -- skill, has no group here
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(MAX(CASE WHEN q.alive_bosses = 0 THEN 1 ELSE 0 END), 0) AS r
      FROM (
        SELECT SUM(CASE WHEN h.alive THEN 1 ELSE 0 END) AS alive_bosses
        FROM boss_actions ba
        JOIN maps m ON m.name = ba.map_name AND m.map_id = a_map_id::int
        JOIN things t ON t.map_id = m.map_id AND t.type = ba.boss_type
        JOIN thing_health h ON h.map_id = t.map_id AND h.thing_id = t.id
        WHERE ba.action = 'lower_666'
        GROUP BY ba.map_name
      ) q;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_r;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_ready = v_r;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_ready <> 0 THEN
    -- EV_DoFloor(line, lowerFloorToLowest) against tag 666
    WITH adjacency AS (
      SELECT sector_id, other_id FROM sector_adjacency
      WHERE map_id=a_map_id::int AND sector_id<>other_id
    ),
    targets AS (
      SELECT s.id AS sector_id, s.floor_height,
             MIN(other.floor_height) AS lowest_floor
      FROM sectors s
      JOIN adjacency a ON a.sector_id=s.id
      JOIN sectors other ON other.map_id=s.map_id AND other.id=a.other_id
      WHERE s.map_id=a_map_id::int AND s.tag=666
      GROUP BY s.id, s.floor_height
    )
    INSERT INTO sector_movers
      (map_id,sector_id,source_line_id,mover_type,plane,direction,
       bottom_height,top_height,speed,wait_tics,countdown,
       next_ceiling,next_floor,target_floor_tex,moved_this_tick)
    SELECT a_map_id::int,sector_id,NULL,'floor_lower','floor',-1,
           lowest_floor::smallint,floor_height,1,0,0,NULL,NULL,NULL,FALSE
    FROM targets
    WHERE lowest_floor < floor_height
    ON CONFLICT (map_id,sector_id) DO NOTHING;
  END IF;
  RETURN v_ready;
END;;

-- ==== 36_finale.sql ====
DROP FUNCTION IF EXISTS doom_finale_begin;;
CREATE FUNCTION doom_finale_begin(a_episode INT) RETURNS INT
BEGIN
  DECLARE v_found BIGINT;
  DECLARE v_n BIGINT;
  -- F_StartFinale. Returns 1 when this episode has a finale to show and 0 when
  -- it does not, which is the caller's cue to go straight back to the title
  -- screen rather than sit on an empty page.
  SET v_found = 0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COUNT(*) AS n FROM finale_defs WHERE episode = a_episode::int;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_n;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_found = v_n;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_found <> 0 THEN
    UPDATE screen_state
      SET screen='finale', finale_episode=a_episode::int,
          finale_count=0, finale_stage=0, cursor_index=0
      WHERE id=0;
  END IF;
  RETURN v_found;
END;;

DROP FUNCTION IF EXISTS doom_finale_tic;;
CREATE FUNCTION doom_finale_tic() RETURNS TEXT
BEGIN
  DECLARE v_result TEXT;
  DECLARE v_r BOOLEAN;
  -- One 35 Hz step of F_Ticker. The page types out at TEXTSPEED (3 tics a
  -- character) and then sits for TEXTWAIT (250 tics) before the episode's
  -- ending picture replaces it.
  --
  -- Returns 'text' while the page is up, 'picture' once the ending art is, and
  -- 'done' if something else has taken the screen.
  SET v_result = 'text';
  UPDATE screen_state s
  SET finale_count = CASE WHEN d.advance THEN 0 ELSE s.finale_count + 1 END,
      finale_stage = CASE WHEN d.advance THEN 1 ELSE s.finale_stage END
  FROM (
    SELECT s2.id,
           (s2.finale_stage = 0
            AND s2.finale_count + 1 > length(f.story_text) * 3 + 250) AS advance
    FROM screen_state s2
    JOIN finale_defs f ON f.episode = s2.finale_episode
    WHERE s2.id = 0 AND s2.screen = 'finale'
  ) d
  WHERE s.id = d.id;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT CASE WHEN screen <> 'finale' THEN 'done'
                  WHEN finale_stage = 0 THEN 'text'
                  ELSE 'picture' END AS r
      FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_r;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_result = v_r;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_result;
END;;

DROP FUNCTION IF EXISTS doom_finale_advance;;
CREATE FUNCTION doom_finale_advance() RETURNS TEXT
BEGIN
  DECLARE v_result TEXT;
  DECLARE v_stage BIGINT;
  DECLARE v_count BIGINT;
  DECLARE v_typed BIGINT;
  DECLARE v_st BIGINT;
  DECLARE v_c BIGINT;
  DECLARE v_t BIGINT;
  -- A keypress during the finale.
  SET v_result = 'text';
  SET v_stage = 0;
  SET v_count = 0;
  SET v_typed = 0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT s.finale_stage AS st, s.finale_count AS c,
             COALESCE(length(f.story_text), 0) * 3 + 10 AS t
      FROM screen_state s
      LEFT JOIN finale_defs f ON f.episode = s.finale_episode
      WHERE s.id = 0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_st, v_c, v_t;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_stage = v_st;
      SET v_count = v_c;
      SET v_typed = v_t;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_stage <> 0 THEN
    UPDATE screen_state
      SET screen='title', finale_stage=0, finale_count=0, finale_episode=NULL,
          cursor_index=0
      WHERE id=0;
    SET v_result = 'done';
  END IF;
  IF v_stage = 0 AND v_count < v_typed THEN
    -- Still typing: put the whole page up at once, leaving the wait to run.
    UPDATE screen_state SET finale_count=v_typed WHERE id=0;
    SET v_result = 'text';
  END IF;
  IF v_stage = 0 AND v_count >= v_typed THEN
    UPDATE screen_state SET finale_stage=1, finale_count=0 WHERE id=0;
    SET v_result = 'picture';
  END IF;
  RETURN v_result;
END;;

-- ==== 37_cs_discover.sql ====
DROP FUNCTION IF EXISTS doom_cs_discover;;
CREATE FUNCTION doom_cs_discover(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  -- Reveal the automap as the player walks it.
  INSERT INTO mapped_lines (map_id, line_id)
  SELECT ps.map_id, ld.id
  FROM player_state ps
  JOIN linedefs ld ON ld.map_id = ps.map_id
  LEFT JOIN sidedefs rs ON rs.map_id = ld.map_id AND rs.id = ld.right_sd_id
  LEFT JOIN sidedefs ls ON ls.map_id = ld.map_id AND ls.id = ld.left_sd_id
  WHERE ps.map_id = a_map_id::int
    AND ps.player_thing_id = a_player_thing_id::int
    AND ps.sector_id IS NOT NULL
    AND (rs.sector_id = ps.sector_id OR ls.sector_id = ps.sector_id)
  ON CONFLICT (map_id, line_id) DO NOTHING;
  RETURN 0;
END;;

-- ==== 38_cheats.sql ====
DROP FUNCTION IF EXISTS doom_cheat;;
CREATE FUNCTION doom_cheat(a_map_id INT, a_player_thing_id INT, a_code TEXT) RETURNS TEXT
BEGIN
  DECLARE v_message TEXT;
  DECLARE v_code TEXT;
  DECLARE v_m TEXT;
  DECLARE v_m_2 TEXT;
  DECLARE v_ignored INT;
  DECLARE v_m_3 TEXT;
  -- Apply one typed cheat code and return the message Doom prints for it.
  --
  -- (p_local.h): INVULNTICS 30 s, INVISTICS and IRONTICS 60 s, INFRATICS 120 s.
  SET v_message = '';
  SET v_code = UPPER(a_code::text);
  IF v_code = 'IDDQD' THEN
    UPDATE player_state SET god_mode = NOT god_mode
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    BEGIN
      DECLARE b1_done INT DEFAULT 0;
      DECLARE b1_rows CURSOR FOR SELECT CASE WHEN god_mode THEN 'Degreelessness Mode ON'
                      ELSE 'Degreelessness Mode OFF' END AS m
          FROM player_state
          WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
      OPEN b1_rows;
      b1_each: LOOP
        FETCH b1_rows INTO v_m;
        IF b1_done THEN LEAVE b1_each; END IF;
        SET v_message = v_m;
      END LOOP;
      CLOSE b1_rows;
    END;
  END IF;
  -- IDSPISPOPD is Doom 1's spelling, IDCLIP Doom 2's. Both are accepted.
  IF v_code = 'IDCLIP' OR v_code = 'IDSPISPOPD' THEN
    UPDATE player_state SET noclip = NOT noclip
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    BEGIN
      DECLARE b2_done INT DEFAULT 0;
      DECLARE b2_rows CURSOR FOR SELECT CASE WHEN noclip THEN 'No Clipping Mode ON'
                      ELSE 'No Clipping Mode OFF' END AS m
          FROM player_state
          WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
      OPEN b2_rows;
      b2_each: LOOP
        FETCH b2_rows INTO v_m_2;
        IF b2_done THEN LEAVE b2_each; END IF;
        SET v_message = v_m_2;
      END LOOP;
      CLOSE b2_rows;
    END;
  END IF;
  IF v_code = 'IDFA' OR v_code = 'IDKFA' THEN
    -- The arsenal itself is the same one the menu cheat grants.
    SET v_ignored = doom_cheat_arsenal(a_map_id, a_player_thing_id);
    SET v_message = 'Ammo (no keys) Added';
  END IF;
  IF v_code = 'IDKFA' THEN
    UPDATE player_state SET key_red=TRUE, key_blue=TRUE, key_yellow=TRUE
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Very Happy Ammo Added';
  END IF;
  IF v_code = 'IDBEHOLDV' THEN
    UPDATE player_state SET invuln_tics=1050
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Invulnerability';
  END IF;
  IF v_code = 'IDBEHOLDS' THEN
    UPDATE player_state SET berserk=TRUE, health=GREATEST(health,100)
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Berserk';
  END IF;
  IF v_code = 'IDBEHOLDI' THEN
    UPDATE player_state SET invis_tics=2100
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Partial Invisibility';
  END IF;
  IF v_code = 'IDBEHOLDR' THEN
    UPDATE player_state SET radsuit_tics=2100
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Radiation Suit';
  END IF;
  IF v_code = 'IDBEHOLDA' THEN
    UPDATE player_state SET power_map=TRUE
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Computer Area Map';
  END IF;
  IF v_code = 'IDBEHOLDL' THEN
    UPDATE player_state SET light_amp_tics=4200
      WHERE map_id=a_map_id::int AND player_thing_id=a_player_thing_id::int;
    SET v_message = 'Light Amplification Visor';
  END IF;
  IF v_code = 'IDCHOPPERS' THEN
    INSERT INTO player_weapon_owned (map_id, player_thing_id, weapon_id)
      VALUES (a_map_id::int, a_player_thing_id::int, 8)
      ON CONFLICT DO NOTHING;
    SET v_message = '... doesn''t suck - GM';
  END IF;
  IF v_code = 'IDMYPOS' THEN
    -- Vanilla prints these as raw fixed-point hex, which is unreadable; map
    -- units rounded to whole numbers say the same thing.
    BEGIN
      DECLARE b3_done INT DEFAULT 0;
      DECLARE b3_rows CURSOR FOR SELECT 'ang=' || ROUND(t.angle)::int::text
              || '; x,y=(' || ROUND(t.x)::int::text
              || ',' || ROUND(t.y)::int::text || ')' AS m
          FROM things t
          WHERE t.map_id=a_map_id::int AND t.id=a_player_thing_id::int;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
      OPEN b3_rows;
      b3_each: LOOP
        FETCH b3_rows INTO v_m_3;
        IF b3_done THEN LEAVE b3_each; END IF;
        SET v_message = v_m_3;
      END LOOP;
      CLOSE b3_rows;
    END;
  END IF;
  RETURN v_message;
END;;

-- ST_Responder: every letter typed in the level goes into a buffer, matched
-- against the sequences from the end. Returns what the client has to act on:
-- 'msg:<text>' (a cheat above was applied), 'warp:ExMy' (IDCLEV), 'music:ExMy'
-- (IDMUS), 'iddt' (the automap's own toggle), or 'none'.
DROP FUNCTION IF EXISTS doom_cheat_key;;
CREATE FUNCTION doom_cheat_key(a_map_id INT, a_player_thing_id INT, a_key TEXT) RETURNS TEXT
BEGIN
  DECLARE v_buf TEXT;
  DECLARE v_b TEXT;
  DECLARE v_result TEXT;
  DECLARE v_r BOOLEAN;
  DECLARE v_code TEXT;
  DECLARE v_msg TEXT;
  UPDATE screen_state SET cheat_buffer = RIGHT(cheat_buffer || UPPER(a_key), 12) WHERE id=0;
  SET v_buf = '';
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT cheat_buffer AS b FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_b;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_buf = v_b;
    END LOOP;
    CLOSE b1_rows;
  END;
  SET v_result = 'none';
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT CASE
        WHEN SUBSTRING(v_buf FROM LENGTH(v_buf)-7 FOR 6) = 'IDCLEV'
             AND SUBSTRING(v_buf FROM LENGTH(v_buf)-1 FOR 1) BETWEEN '1' AND '9'
             AND RIGHT(v_buf,1) BETWEEN '1' AND '9'
          THEN 'warp:E' || SUBSTRING(v_buf FROM LENGTH(v_buf)-1 FOR 1) || 'M' || RIGHT(v_buf,1)
        WHEN SUBSTRING(v_buf FROM LENGTH(v_buf)-6 FOR 5) = 'IDMUS'
             AND SUBSTRING(v_buf FROM LENGTH(v_buf)-1 FOR 1) BETWEEN '1' AND '9'
             AND RIGHT(v_buf,1) BETWEEN '1' AND '9'
          THEN 'music:E' || SUBSTRING(v_buf FROM LENGTH(v_buf)-1 FOR 1) || 'M' || RIGHT(v_buf,1)
        WHEN RIGHT(v_buf,4) = 'IDDT' THEN 'iddt'
        ELSE COALESCE((
          SELECT 'code:' || c.code
          FROM (VALUES ('IDSPISPOPD'),('IDCHOPPERS'),('IDBEHOLDV'),('IDBEHOLDS'),
                       ('IDBEHOLDI'),('IDBEHOLDR'),('IDBEHOLDA'),('IDBEHOLDL'),
                       ('IDMYPOS'),('IDCLIP'),('IDKFA'),('IDDQD'),('IDFA')) AS c(code)
          WHERE v_buf LIKE '%' || c.code
          ORDER BY LENGTH(c.code) DESC, c.code LIMIT 1), 'none')
      END AS r;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_r;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_result = v_r;
    END LOOP;
    CLOSE b2_rows;
  END;
  IF v_result LIKE 'code:%' THEN
    SET v_code = SUBSTRING(v_result FROM 6);
    SET v_msg = doom_cheat(a_map_id, a_player_thing_id, v_code);
    SET v_result = 'msg:' || v_msg;
  END IF;
  IF v_result <> 'none' THEN
    UPDATE screen_state SET cheat_buffer='' WHERE id=0;
  END IF;
  RETURN v_result;
END;;

-- ==== 39_mp.sql ====
-- ---------------------------------------------------------------------------
-- G_DeathMatchSpawnPlayer: put the player on a random deathmatch start that no
-- other player is standing on, with the vanilla reborn loadout.
DROP FUNCTION IF EXISTS doom_mp_respawn;;
CREATE FUNCTION doom_mp_respawn(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_respawn_fog_base DOUBLE;
  DECLARE v_tic_span DOUBLE;
  SET v_respawn_fog_base = doom_const('PLAYER_RESPAWN_EFFECT_ID_BASE');
  SET v_tic_span = doom_const('EFFECT_ID_TIC_SPAN');
  WITH me AS (
    SELECT level_tics FROM player_state
    WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id
  ),
  others AS (
    SELECT t.x::float8 AS x, t.y::float8 AS y
    FROM player_state ps
    JOIN things t ON t.map_id=ps.map_id AND t.id=ps.player_thing_id
    WHERE ps.map_id=a_map_id AND ps.player_thing_id<>a_player_thing_id AND ps.alive
  ),
  starts AS (
    -- Thing type 11 is a deathmatch start. P_CheckSpot would refuse one with a
    -- body in it; a 64-unit box around every other live player stands in.
    SELECT t.id, t.x, t.y, t.angle,
           ROW_NUMBER() OVER (ORDER BY t.id) AS rn, COUNT(*) OVER () AS n
    FROM things t
    WHERE t.map_id=a_map_id AND t.type=11
      AND NOT EXISTS (SELECT 1 FROM others o
                      WHERE ABS(o.x-t.x)<64.0 AND ABS(o.y-t.y)<64.0)
  ),
  pick AS (
    SELECT s.* FROM starts s CROSS JOIN me
    WHERE s.rn = (doom_prandom(a_player_thing_id, me.level_tics, 41) % s.n) + 1
  )
  UPDATE things t
  SET x=pk.x, y=pk.y, angle=pk.angle, mom_x=0, mom_y=0
  FROM pick pk
  WHERE t.map_id=a_map_id AND t.id=a_player_thing_id;
  -- G_PlayerReborn: full health, no armour, fist and pistol with 50 bullets.
  -- The frag count is the one thing that survives.
  UPDATE player_state ps
  SET health=100, alive=TRUE, armor=0, armor_class=0, backpack=FALSE,
      ammo_bullets=50, ammo_shells=0, ammo_rockets=0, ammo_cells=0,
      key_blue=FALSE, key_yellow=FALSE, key_red=FALSE,
      radsuit_tics=0, invis_tics=0, light_amp_tics=0, invuln_tics=0,
      berserk=FALSE, power_map=FALSE, god_mode=FALSE, noclip=FALSE,
      pain_face_tics=0, damage_count=0, bonus_count=0,
      death_tics=0, killer_id=NULL, sprite_frame='A',
      momentum_x=0, momentum_y=0, momentum_z=0, bob_strength=0
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id;
  DELETE FROM player_weapon_owned
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id;
  INSERT INTO player_weapon_owned (map_id, player_thing_id, weapon_id)
  VALUES (a_map_id, a_player_thing_id, 1), (a_map_id, a_player_thing_id, 2);
  DELETE FROM player_weapons
  WHERE map_id=a_map_id AND player_thing_id=a_player_thing_id;
  INSERT INTO player_weapons (map_id, player_thing_id)
  VALUES (a_map_id, a_player_thing_id);
  -- The death cue is latched per player so it plays once per death; a new life
  -- needs it released.
  DELETE FROM sound_events
  WHERE map_id=a_map_id AND event_key='plr-death:'||a_player_thing_id::text;
  DO doom_spawn_player(a_map_id, a_player_thing_id);
  -- The teleport fog at the new spot, and its sound.
  INSERT INTO world_effects (map_id, effect_id, effect_type, x, y, z, sector_id, age)
  SELECT ps.map_id, v_respawn_fog_base::bigint + ps.player_thing_id::bigint * v_tic_span
                    + (ps.level_tics % v_tic_span),
         'tfog', ps.position_x, ps.position_y, s.floor_height, ps.sector_id, 0
  FROM player_state ps
  JOIN sectors s ON s.map_id=ps.map_id AND s.id=ps.sector_id
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id
  ON CONFLICT (map_id, effect_id) DO NOTHING;
  INSERT INTO sound_events
    (map_id,event_key,level_tic,sound_name,source_thing_id,source_x,source_y)
  SELECT ps.map_id,'respawn:'||ps.player_thing_id::text||':'||ps.level_tics::text,
         ps.level_tics,'DSTELEPT',ps.player_thing_id,ps.position_x,ps.position_y
  FROM player_state ps
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id
  ON CONFLICT (map_id,event_key) DO NOTHING;
  RETURN 1;
END;;

-- ---------------------------------------------------------------------------
-- Start a two-player deathmatch on a map: the single-player reset, a second
-- player on the player-2 start's Thing, -nomonsters, and both players placed
-- on deathmatch starts.
DROP FUNCTION IF EXISTS doom_mp_vacate_all;;
CREATE FUNCTION doom_mp_vacate_all(a_map_id INT) RETURNS INT
BEGIN
  UPDATE player_state ps
  SET alive=FALSE, health=0, killer_id=NULL, death_tics=36,
      momentum_x=0, momentum_y=0, momentum_z=0
  FROM mp_players mp
  WHERE mp.map_id=ps.map_id AND mp.player_thing_id=ps.player_thing_id
    AND mp.map_id=a_map_id;
  UPDATE mp_players SET role_name=NULL, last_seen=NULL, display_name=NULL WHERE map_id=a_map_id;
  DELETE FROM mp_join_requests WHERE map_id=a_map_id;
  RETURN 1;
END;;

-- Hand back every slot whose holder has said nothing for p_seconds: a closed
-- tab, a dropped connection. Returns how many were freed.
DROP FUNCTION IF EXISTS doom_mp_start;;
CREATE FUNCTION doom_mp_start(a_map_id INT, a_skill INT) RETURNS INT
BEGIN
  DECLARE v_monsters BOOLEAN;
  DECLARE v_x BOOLEAN;
  DECLARE v_q1 INT;
  DECLARE v_q2 INT;
  DECLARE v_q3 INT;
  DECLARE v_q4 INT;
  DECLARE v_player_thing_id BIGINT;
  DECLARE v_player_thing_id_2 BIGINT;
  DECLARE v_player_thing_id_3 BIGINT;
  DECLARE v_player_thing_id_4 BIGINT;
  DECLARE v_n BIGINT;
  DECLARE v_c INT;
  DO doom_reset_stage(a_map_id, a_skill);
  -- One match at a time: every slot of every map is cleared.
  DELETE FROM mp_players;
  -- Slot n is the player-n start's Thing: the row the player IS, wherever the
  -- deathmatch start puts it. Slots open free; api_join claims them.
  INSERT INTO mp_players (map_id, slot, player_thing_id, skill)
  SELECT a_map_id, t.type, MIN(t.id), a_skill
  FROM things t WHERE t.map_id=a_map_id AND t.type IN (1,2,3,4)
  GROUP BY t.type;
  -- doom_reset_stage created the player rows for the type-1 start; the same
  -- rows, same shape, for slots 2 to 4.
  INSERT INTO player_weapons (map_id, player_thing_id)
  SELECT map_id, player_thing_id FROM mp_players WHERE map_id=a_map_id AND slot>=2;
  INSERT INTO player_weapon_owned (map_id, player_thing_id, weapon_id)
  SELECT mp.map_id, mp.player_thing_id, w.weapon_id
  FROM mp_players mp CROSS JOIN (VALUES (1), (2)) AS w (weapon_id)
  WHERE mp.map_id=a_map_id AND mp.slot>=2;
  INSERT INTO player_state (
    map_id,player_thing_id,health,alive,ammo_bullets,
    previous_x,previous_y,position_x,position_y,base_z,view_z,view_angle
  )
  SELECT t.map_id,t.id,100,TRUE,50,t.x,t.y,t.x,t.y,t.z,t.z,t.angle
  FROM mp_players mp
  JOIN things t ON t.map_id=mp.map_id AND t.id=mp.player_thing_id
  WHERE mp.map_id=a_map_id AND mp.slot>=2;
  -- -nomonsters, unless the match keeps them: everything that would count as
  -- a kill is not spawned. Barrels stay. A Thing without health or AI would
  -- still be drawn standing there by the renderer, so it is hidden the way a
  -- taken item is.
  SET v_monsters = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT m.monsters AS x FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_monsters = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF NOT v_monsters THEN
    INSERT INTO picked_up_items (map_id, thing_id)
    SELECT h.map_id, h.thing_id
    FROM thing_health h
    JOIN things t ON t.map_id=h.map_id AND t.id=h.thing_id
    JOIN thing_combat_defs d ON d.thing_type=t.type AND d.counts_kill
    WHERE h.map_id=a_map_id
    ON CONFLICT (map_id, thing_id) DO NOTHING;
    DELETE FROM monster_ai ai
    USING things t, thing_combat_defs d
    WHERE ai.map_id=a_map_id AND t.map_id=ai.map_id AND t.id=ai.thing_id
      AND d.thing_type=t.type AND d.counts_kill;
    DELETE FROM thing_health h
    USING things t, thing_combat_defs d
    WHERE h.map_id=a_map_id AND t.map_id=h.map_id AND t.id=h.thing_id
      AND d.thing_type=t.type AND d.counts_kill;
  END IF;
  DELETE FROM mp_inputs WHERE map_id=a_map_id;
  -- Every slot gets a reborn loadout at a deathmatch start, then lies dead and
  -- free: no input ever reaches an unclaimed slot, so P_DeathThink never
  -- respawns it, and the claim (api_join, via the next tic) respawns it fresh.
  SET v_q1 = NULL::int;
  SET v_q2 = NULL::int;
  SET v_q3 = NULL::int;
  SET v_q4 = NULL::int;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT player_thing_id FROM mp_players WHERE map_id=a_map_id AND slot=1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_player_thing_id;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_q1 = v_player_thing_id;
    END LOOP;
    CLOSE b2_rows;
  END;
  BEGIN
    DECLARE b3_done INT DEFAULT 0;
    DECLARE b3_rows CURSOR FOR SELECT player_thing_id FROM mp_players WHERE map_id=a_map_id AND slot=2;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
    OPEN b3_rows;
    b3_each: LOOP
      FETCH b3_rows INTO v_player_thing_id_2;
      IF b3_done THEN LEAVE b3_each; END IF;
      SET v_q2 = v_player_thing_id_2;
    END LOOP;
    CLOSE b3_rows;
  END;
  BEGIN
    DECLARE b4_done INT DEFAULT 0;
    DECLARE b4_rows CURSOR FOR SELECT player_thing_id FROM mp_players WHERE map_id=a_map_id AND slot=3;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b4_done = 1;
    OPEN b4_rows;
    b4_each: LOOP
      FETCH b4_rows INTO v_player_thing_id_3;
      IF b4_done THEN LEAVE b4_each; END IF;
      SET v_q3 = v_player_thing_id_3;
    END LOOP;
    CLOSE b4_rows;
  END;
  BEGIN
    DECLARE b5_done INT DEFAULT 0;
    DECLARE b5_rows CURSOR FOR SELECT player_thing_id FROM mp_players WHERE map_id=a_map_id AND slot=4;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b5_done = 1;
    OPEN b5_rows;
    b5_each: LOOP
      FETCH b5_rows INTO v_player_thing_id_4;
      IF b5_done THEN LEAVE b5_each; END IF;
      SET v_q4 = v_player_thing_id_4;
    END LOOP;
    CLOSE b5_rows;
  END;
  IF v_q1 IS NOT NULL THEN
    DO doom_mp_respawn(a_map_id, v_q1);
  END IF;
  IF v_q2 IS NOT NULL THEN
    DO doom_mp_respawn(a_map_id, v_q2);
  END IF;
  IF v_q3 IS NOT NULL THEN
    DO doom_mp_respawn(a_map_id, v_q3);
  END IF;
  IF v_q4 IS NOT NULL THEN
    DO doom_mp_respawn(a_map_id, v_q4);
  END IF;
  DO doom_mp_vacate_all(a_map_id);
  SET v_n = 0;
  BEGIN
    DECLARE b6_done INT DEFAULT 0;
    DECLARE b6_rows CURSOR FOR SELECT count(*)::int AS c FROM mp_players WHERE map_id=a_map_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b6_done = 1;
    OPEN b6_rows;
    b6_each: LOOP
      FETCH b6_rows INTO v_c;
      IF b6_done THEN LEAVE b6_each; END IF;
      SET v_n = v_c;
    END LOOP;
    CLOSE b6_rows;
  END;
  RETURN v_n;
END;;

-- ---------------------------------------------------------------------------
-- The PLAY sprite frame the other client draws this player with. Vanilla runs
-- the player mobj through S_PLAY_RUN1-4 (A-D, 4 tics each) while moving,
-- S_PLAY_ATK1/2 (E, then F with the muzzle flash) when firing, S_PLAY_PAIN (G)
-- for 8 tics after a hit, S_PLAY_DIE1-7 (H-N, 10 tics each) or, gibbed,
-- S_PLAY_XDIE1-9 (O-W, 5 tics each).
DROP FUNCTION IF EXISTS doom_cs_player_pose;;
CREATE FUNCTION doom_cs_player_pose(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  UPDATE player_state ps
  SET sprite_frame = (CASE
    WHEN NOT ps.alive THEN
      CASE WHEN ps.health < -100
           THEN CHR(ASCII('O') + LEAST(8, ps.death_tics / 5))
           ELSE CHR(ASCII('H') + LEAST(6, ps.death_tics / 10)) END
    WHEN ps.pain_face_tics >= 9 THEN 'G'
    WHEN pw.flash_seq_index IS NOT NULL THEN 'F'
    WHEN pw.state NOT IN ('ready','up','down') THEN 'E'
    WHEN ps.bob_strength > 0.05 OR ABS(ps.momentum_x) + ABS(ps.momentum_y) > 0.1
      THEN SUBSTRING('ABCD' FROM ((ps.level_tics / 4) % 4)::int + 1 FOR 1)
    ELSE 'A' END)::char(1)
  FROM player_weapons pw
  WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_player_thing_id
    AND pw.map_id=ps.map_id AND pw.player_thing_id=ps.player_thing_id;
  RETURN 0;
END;;

-- ---------------------------------------------------------------------------
-- P_KillMobj's frag bookkeeping, once per tic over everyone who died in it.
-- Thing ids start at 0, so the world is -1 and every non-negative killer is a
-- player.
-- The damage statements leave the killer in player_state.killer_id: another
-- player's thing id, or -1 for the world (a barrel, a floor, a monster).
-- Killing yourself or dying to the world costs a frag, as in vanilla.
DROP FUNCTION IF EXISTS doom_mp_frags;;
CREATE FUNCTION doom_mp_frags(a_map_id INT) RETURNS INT
BEGIN
  UPDATE player_state k
  SET frags = k.frags + 1,
      message = 'YOU FRAGGED ' || COALESCE(vs.display_name, 'PLAYER ' || vs.slot::text),
      message_tics = 140
  FROM player_state v
  JOIN mp_players vs ON vs.map_id=v.map_id AND vs.player_thing_id=v.player_thing_id
  WHERE v.map_id=a_map_id AND NOT v.alive AND v.killer_id IS NOT NULL
    AND v.killer_id >= 0 AND v.killer_id <> v.player_thing_id
    AND k.map_id=v.map_id AND k.player_thing_id=v.killer_id;
  UPDATE player_state v
  SET frags = v.frags - CASE WHEN v.killer_id = v.player_thing_id OR v.killer_id < 0
                             THEN 1 ELSE 0 END,
      message = CASE WHEN v.killer_id = v.player_thing_id THEN 'YOU KILLED YOURSELF'
                     WHEN v.killer_id < 0 THEN 'YOU DIED'
                     ELSE COALESCE(ks.display_name, 'PLAYER ' || ks.slot::text) || ' FRAGGED YOU' END,
      message_tics = 140,
      killer_id = NULL
  FROM mp_players ks
  WHERE v.map_id=a_map_id AND NOT v.alive AND v.killer_id IS NOT NULL
    AND ks.map_id=v.map_id
    AND ks.player_thing_id = CASE WHEN v.killer_id >= 0 THEN v.killer_id
                                  ELSE v.player_thing_id END;
  RETURN 0;
END;;

-- A slot with nobody behind it. The player is left dead with its frags: the
-- respawn on the next claim gives it a fresh body, and nothing can score off
-- a corpse.
DROP FUNCTION IF EXISTS doom_mp_reap;;
CREATE FUNCTION doom_mp_reap(a_map_id INT, a_seconds INT) RETURNS INT
BEGIN
  DECLARE v_freed BIGINT;
  DECLARE v_playing BOOLEAN;
  DECLARE v_x BOOLEAN;
  DECLARE v_n INT;
  DELETE FROM mp_waiting WHERE EXTRACT(EPOCH FROM (now() - last_seen)) > 5;
  SET v_freed = 0;
  -- Nobody can heartbeat between maps (the API is quiet then), so nobody is
  -- idle either.
  SET v_playing = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT (m.state = 'playing') AS x FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_playing = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_playing THEN
    BEGIN
      DECLARE b2_done INT DEFAULT 0;
      DECLARE b2_rows CURSOR FOR SELECT count(*)::int AS n FROM mp_players mp
        WHERE mp.map_id=a_map_id AND mp.role_name IS NOT NULL
          AND (mp.last_seen IS NULL OR EXTRACT(EPOCH FROM (now() - mp.last_seen)) > a_seconds);
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
      OPEN b2_rows;
      b2_each: LOOP
        FETCH b2_rows INTO v_n;
        IF b2_done THEN LEAVE b2_each; END IF;
        SET v_freed = v_n;
      END LOOP;
      CLOSE b2_rows;
    END;
  END IF;
  IF v_freed > 0 THEN
    UPDATE player_state ps
      SET alive=FALSE, health=0, killer_id=NULL, death_tics=36,
          momentum_x=0, momentum_y=0, momentum_z=0
      FROM mp_players mp
      WHERE mp.map_id=ps.map_id AND mp.player_thing_id=ps.player_thing_id
        AND mp.map_id=a_map_id AND mp.role_name IS NOT NULL
        AND (mp.last_seen IS NULL OR EXTRACT(EPOCH FROM (now() - mp.last_seen)) > a_seconds);
    UPDATE mp_players SET role_name=NULL, last_seen=NULL, display_name=NULL
      WHERE map_id=a_map_id AND role_name IS NOT NULL
        AND (last_seen IS NULL OR EXTRACT(EPOCH FROM (now() - last_seen)) > a_seconds);
  END IF;
  RETURN v_freed;
END;;

-- One referee per map. TRUE when this caller now holds the map: no heartbeat
-- yet, or the last one is older than p_stale_seconds (a referee that died).
DROP FUNCTION IF EXISTS doom_mp_referee_claim;;
CREATE FUNCTION doom_mp_referee_claim(a_map_id INT, a_stale_seconds INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_busy BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_busy = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS(SELECT 1 FROM mp_referee r WHERE r.map_id=a_map_id
                    AND EXTRACT(EPOCH FROM (now() - r.heartbeat)) < a_stale_seconds) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_busy = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF NOT v_busy THEN
    DELETE FROM mp_referee WHERE map_id=a_map_id;
    INSERT INTO mp_referee (map_id, heartbeat) VALUES (a_map_id, now());
  END IF;
  RETURN NOT v_busy;
END;;

DROP FUNCTION IF EXISTS doom_mp_referee_beat;;
CREATE FUNCTION doom_mp_referee_beat(a_map_id INT) RETURNS INT
BEGIN
  UPDATE mp_referee SET heartbeat=now() WHERE map_id=a_map_id;
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_mp_referee_release;;
CREATE FUNCTION doom_mp_referee_release(a_map_id INT) RETURNS INT
BEGIN
  DELETE FROM mp_referee WHERE map_id=a_map_id;
  RETURN 1;
END;;

-- Ask the next tic to spawn every slot, so the referee can run the join path
-- once before anyone arrives: its first execution compiles for seconds, and
-- that stall must not land on the first player's join.
DROP FUNCTION IF EXISTS doom_mp_request_all;;
CREATE FUNCTION doom_mp_request_all(a_map_id INT) RETURNS INT
BEGIN
  INSERT INTO mp_join_requests (map_id, slot)
  SELECT mp.map_id, mp.slot FROM mp_players mp WHERE mp.map_id=a_map_id
  ON CONFLICT DO NOTHING;
  RETURN 1;
END;;

-- ---------------------------------------------------------------------------
-- The match: vanilla's command line as a row. -timer minutes become tics
-- (0: the level never times out), -altdeath, -nomonsters, -respawn.
DROP FUNCTION IF EXISTS doom_mp_configure;;
CREATE FUNCTION doom_mp_configure(a_map_id INT, a_skill INT, a_timer_tics INT, a_altdeath BOOLEAN, a_monsters BOOLEAN, a_respawn_monsters BOOLEAN, a_intermission_tics INT) RETURNS INT
BEGIN
  DELETE FROM mp_match;
  -- Closed until doom_mp_open: the referee opens the first map and compiles
  -- the tic in peace, and no client writes meanwhile.
  INSERT INTO mp_match (id, map_id, skill, state, timer_tics, altdeath, monsters, respawn_monsters, intermission_tics, intermission_until)
  VALUES (0, a_map_id, a_skill, 'intermission', a_timer_tics, a_altdeath, a_monsters, a_respawn_monsters, a_intermission_tics, now());
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_mp_open;;
CREATE FUNCTION doom_mp_open() RETURNS INT
BEGIN
  UPDATE mp_match SET state = 'playing', intermission_until = NULL;
  RETURN 1;
END;;

-- G_ExitLevel's reasons, for a deathmatch: -timer ran out, or any player
-- used or crossed an exit line, stood on the E1M8 boss floor worn down to
-- it, or the map's boss died
DROP FUNCTION IF EXISTS doom_mp_level_done;;
CREATE FUNCTION doom_mp_level_done(a_map_id INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_playing BOOLEAN;
  DECLARE v_x BOOLEAN;
  DECLARE v_done BOOLEAN;
  DECLARE v_d BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_playing = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT (m.state = 'playing') AS x FROM mp_match m WHERE m.map_id = a_map_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_playing = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  SET v_done = false;
  IF v_playing THEN
    BEGIN
      DECLARE b2_done INT DEFAULT 0;
      DECLARE b2_rows CURSOR FOR SELECT (m.timer_tics > 0 AND COALESCE(MAX(ps.level_tics), 0) >= m.timer_tics) AS d
          FROM mp_match m
          LEFT JOIN mp_players mp ON mp.map_id = m.map_id
          LEFT JOIN player_state ps ON ps.map_id = mp.map_id AND ps.player_thing_id = mp.player_thing_id
          WHERE m.map_id = a_map_id
          GROUP BY m.timer_tics;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
      OPEN b2_rows;
      b2_each: LOOP
        FETCH b2_rows INTO v_d;
        IF b2_done THEN LEAVE b2_each; END IF;
        SET v_done = v_d;
      END LOOP;
      CLOSE b2_rows;
    END;
  END IF;
  IF v_playing AND NOT v_done THEN
    BEGIN
      DECLARE b3_done INT DEFAULT 0;
      DECLARE b3_rows CURSOR FOR SELECT EXISTS (
            SELECT 1 FROM line_special_events e
            JOIN mp_players mp ON mp.map_id = e.map_id AND mp.player_thing_id = e.player_thing_id
            JOIN linedefs ld ON ld.map_id = e.map_id AND ld.id = e.line_id
            JOIN line_special_defs d ON d.special = ld.special
            WHERE e.map_id = a_map_id AND e.trigger_type = 'cross' AND d.is_exit
            UNION ALL
            SELECT 1 FROM line_use_results ur
            JOIN mp_players mp ON mp.map_id = ur.map_id AND mp.player_thing_id = ur.player_thing_id
            JOIN game_tic_commands c ON c.map_id = ur.map_id AND c.player_thing_id = ur.player_thing_id
            JOIN line_special_defs d ON d.special = ur.special
            WHERE ur.map_id = a_map_id AND ur.eligible AND c.use_requested AND d.is_exit
            UNION ALL
            SELECT 1 FROM player_state ps
            JOIN mp_players mp ON mp.map_id = ps.map_id AND mp.player_thing_id = ps.player_thing_id
            JOIN sectors s ON s.map_id = ps.map_id AND s.id = ps.sector_id
            JOIN sector_special_defs sd ON sd.special = s.special
            WHERE ps.map_id = a_map_id AND mp.role_name IS NOT NULL
              AND ps.health <= sd.exit_at_health
            UNION ALL
            SELECT 1 FROM (
              SELECT SUM(CASE WHEN h.alive THEN 1 ELSE 0 END) AS alive_bosses
              FROM boss_actions ba
              JOIN maps m ON m.name = ba.map_name AND m.map_id = a_map_id
              JOIN things t ON t.map_id = m.map_id AND t.type = ba.boss_type
              JOIN thing_health h ON h.map_id = t.map_id AND h.thing_id = t.id
              WHERE ba.action = 'exit_level'
              GROUP BY ba.map_name) q
            WHERE q.alive_bosses = 0
          ) AS e;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
      OPEN b3_rows;
      b3_each: LOOP
        FETCH b3_rows INTO v_e;
        IF b3_done THEN LEAVE b3_each; END IF;
        SET v_done = v_e;
      END LOOP;
      CLOSE b3_rows;
    END;
  END IF;
  RETURN v_done;
END;;

-- WI_Start for a deathmatch: the world stops (the referee stops ticking), the
-- frag table is up, and the next map follows after intermission_tics.
DROP FUNCTION IF EXISTS doom_mp_intermission_begin;;
CREATE FUNCTION doom_mp_intermission_begin() RETURNS INT
BEGIN
  UPDATE mp_match
  SET state = 'intermission',
      intermission_until = now() + (intermission_tics::double precision / 35.0) * interval '1 second'
  WHERE state = 'playing';
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_mp_intermission_over;;
CREATE FUNCTION doom_mp_intermission_over() RETURNS BOOLEAN
BEGIN
  DECLARE v_over BOOLEAN;
  DECLARE v_x BOOLEAN;
  SET v_over = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT (m.state = 'intermission' AND m.intermission_until <= now()) AS x FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_over = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_over;
END;;

-- The map after the current one in mp_rotation, wrapping to the first
DROP FUNCTION IF EXISTS doom_mp_next_map;;
CREATE FUNCTION doom_mp_next_map() RETURNS INT
BEGIN
  DECLARE v_cur_name TEXT;
  DECLARE v_next_id BIGINT;
  DECLARE v_n TEXT;
  DECLARE v_id BIGINT;
  DECLARE v_id_2 BIGINT;
  DECLARE v_id_3 BIGINT;
  SET v_cur_name = '';
  SET v_next_id = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT ma.name AS n FROM mp_match m JOIN maps ma ON ma.map_id = m.map_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_n;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_cur_name = v_n;
    END LOOP;
    CLOSE b1_rows;
  END;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT ma.map_id AS id
      FROM mp_rotation r JOIN maps ma ON ma.name = r.map_name
      WHERE r.position > COALESCE((SELECT r2.position FROM mp_rotation r2 WHERE r2.map_name = v_cur_name), -1)
      ORDER BY r.position LIMIT 1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_id;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_next_id = v_id;
    END LOOP;
    CLOSE b2_rows;
  END;
  IF v_next_id < 0 THEN
    BEGIN
      DECLARE b3_done INT DEFAULT 0;
      DECLARE b3_rows CURSOR FOR SELECT ma.map_id AS id FROM mp_rotation r JOIN maps ma ON ma.name = r.map_name
          ORDER BY r.position LIMIT 1;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
      OPEN b3_rows;
      b3_each: LOOP
        FETCH b3_rows INTO v_id_2;
        IF b3_done THEN LEAVE b3_each; END IF;
        SET v_next_id = v_id_2;
      END LOOP;
      CLOSE b3_rows;
    END;
  END IF;
  IF v_next_id < 0 THEN
    BEGIN
      DECLARE b4_done INT DEFAULT 0;
      DECLARE b4_rows CURSOR FOR SELECT m.map_id AS id FROM mp_match m;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b4_done = 1;
      OPEN b4_rows;
      b4_each: LOOP
        FETCH b4_rows INTO v_id_3;
        IF b4_done THEN LEAVE b4_each; END IF;
        SET v_next_id = v_id_3;
      END LOOP;
      CLOSE b4_rows;
    END;
  END IF;
  RETURN v_next_id;
END;;

-- G_DoCompleted -> G_DoLoadLevel for the match: the next map is opened with
-- fresh slots, and everyone who held a slot holds the same one there, to be
-- respawned by the first tic (a join request, like a fresh claim). Frags
-- start at zero per map, as vanilla's frag table does. Returns the new map.
DROP FUNCTION IF EXISTS doom_mp_rotate;;
CREATE FUNCTION doom_mp_rotate() RETURNS INT
BEGIN
  DECLARE v_nxt INT;
  DECLARE v_skill BIGINT;
  DECLARE v_s BIGINT;
  SET v_nxt = doom_mp_next_map();
  SET v_skill = 3;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT m.skill AS s FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_s;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_skill = v_s;
    END LOOP;
    CLOSE b1_rows;
  END;
  INSERT INTO mp_holders (slot, role_name, display_name, claimed_at)
  SELECT mp.slot, mp.role_name, mp.display_name, mp.claimed_at FROM mp_players mp WHERE mp.role_name IS NOT NULL
  ON CONFLICT (slot) DO UPDATE SET role_name = EXCLUDED.role_name, display_name = EXCLUDED.display_name,
    claimed_at = EXCLUDED.claimed_at;
  DELETE FROM mp_holders h
  WHERE NOT EXISTS (SELECT 1 FROM mp_players mp WHERE mp.slot = h.slot AND mp.role_name IS NOT NULL);
  DO doom_mp_start(v_nxt, v_skill);
  -- The seat follows the player, and so does the time they have held it.
  UPDATE mp_players mp
  SET role_name = h.role_name, display_name = h.display_name, claimed_at = h.claimed_at,
      last_seen = now() + interval '15 seconds'
  FROM mp_holders h
  WHERE mp.map_id = v_nxt AND mp.slot = h.slot;
  INSERT INTO mp_join_requests (map_id, slot)
  SELECT v_nxt, h.slot FROM mp_holders h
  WHERE EXISTS (SELECT 1 FROM mp_players mp WHERE mp.map_id = v_nxt AND mp.slot = h.slot)
  ON CONFLICT DO NOTHING;
  DELETE FROM mp_inputs;
  UPDATE mp_match
  SET map_id = v_nxt, state = 'playing', intermission_until = NULL, maps_played = maps_played + 1;
  RETURN v_nxt;
END;;

-- What the referee prints every few seconds: the match and the world in one
-- row. Owner-only; every column is a small aggregate over the current map.
CREATE OR REPLACE VIEW mp_stats AS
SELECT m.map_id, ma.name AS map_name, m.state, m.timer_tics, m.maps_played,
       (SELECT COALESCE(MAX(ps.level_tics), 0) FROM player_state ps JOIN mp_players mp
         ON mp.map_id = ps.map_id AND mp.player_thing_id = ps.player_thing_id
         WHERE mp.map_id = m.map_id) AS level_tics,
       (SELECT count(*) FROM thing_health h JOIN things t ON t.map_id = h.map_id AND t.id = h.thing_id
         JOIN thing_combat_defs d ON d.thing_type = t.type AND d.counts_kill
         WHERE h.map_id = m.map_id AND h.alive) AS monsters_alive,
       (SELECT count(*) FROM thing_health h JOIN things t ON t.map_id = h.map_id AND t.id = h.thing_id
         JOIN thing_combat_defs d ON d.thing_type = t.type AND d.counts_kill
         WHERE h.map_id = m.map_id) AS monsters_total,
       (SELECT count(*) FROM monster_ai ai JOIN thing_health h ON h.map_id = ai.map_id AND h.thing_id = ai.thing_id
         WHERE ai.map_id = m.map_id AND h.alive AND ai.state <> 'stand') AS monsters_awake,
       (SELECT count(*) FROM monster_projectiles p WHERE p.map_id = m.map_id) AS projectiles,
       (SELECT count(*) FROM world_effects e WHERE e.map_id = m.map_id) AS effects,
       (SELECT count(*) FROM picked_up_items pu JOIN things t ON t.map_id = pu.map_id AND t.id = pu.thing_id
         JOIN pickup_defs d ON d.thing_type = t.type WHERE pu.map_id = m.map_id) AS items_out,
       (SELECT count(*) FROM item_respawns r WHERE r.map_id = m.map_id) AS items_queued,
       (SELECT count(*) FROM sector_movers sm WHERE sm.map_id = m.map_id) AS movers,
       (SELECT count(*) FROM mp_inputs i) AS input_backlog,
       (SELECT count(*) FROM sound_events e WHERE e.map_id = m.map_id) AS sound_rows,
       (SELECT count(*) FROM pg_stat_activity a WHERE a.state IS NOT NULL) AS sessions,
       (SELECT count(*) FROM mp_waiting w WHERE EXTRACT(EPOCH FROM (now() - w.last_seen)) <= 5) AS waiting
FROM mp_match m JOIN maps ma ON ma.map_id = m.map_id;;

-- ==== 40_run_game_tic.sql ====
-- P_UseLines, when the plan says a use press is pending.
DROP FUNCTION IF EXISTS doom_tic_use;;
CREATE FUNCTION doom_tic_use(a_map_id INT, a_p INT, a_plan BIGINT) RETURNS BOOLEAN
BEGIN
  DECLARE v_queued BOOLEAN;
  SET v_queued = false;
  IF (a_plan & 1) <> 0 THEN
    SET v_queued = doom_cs_use(a_map_id,a_p);
  END IF;
  RETURN v_queued;
END;;

-- Movement: 1 = full move ran, 2 = turn only, 0 = nothing.
DROP FUNCTION IF EXISTS doom_tic_move;;
CREATE FUNCTION doom_tic_move(a_map_id INT, a_p INT) RETURNS INT
BEGIN
  DECLARE v_mode TEXT;
  DECLARE v_did BIGINT;
  SET v_mode = doom_cs_movement_mode(a_map_id,a_p);
  SET v_did = 0;
  IF v_mode = 'full' THEN
    DO doom_cs_move(a_map_id,a_p);
    SET v_did = 1;
  END IF;
  IF v_mode = 'turn' THEN
    DO doom_cs_turn(a_map_id,a_p);
    SET v_did = 2;
  END IF;
  RETURN v_did;
END;;

-- Secret sectors, automap discovery, line crossings and pickups ride the
-- movement gate (plan bit 8): they only have anything to say when the player
-- has changed position. Returns the re-planned bits.
DROP FUNCTION IF EXISTS doom_tic_secrets;;
CREATE FUNCTION doom_tic_secrets(a_map_id INT, a_p INT, a_plan BIGINT) RETURNS BIGINT
BEGIN
  DECLARE v_out BIGINT;
  SET v_out = a_plan;
  IF (a_plan & 8) <> 0 THEN
    DO doom_cs_secret(a_map_id,a_p);
    DO doom_cs_discover(a_map_id,a_p);
    DO doom_cs_cross(a_map_id,a_p);
    DO doom_cs_pickups(a_map_id,a_p);
    SET v_out = doom_cs_plan(a_map_id,a_p);
  END IF;
  RETURN v_out;
END;;

-- Weapon state machine and, when it fired a hitscan weapon, the shot.
-- Returns the re-planned bits (bit 32 stays set once the shot has run).
DROP FUNCTION IF EXISTS doom_tic_weapon;;
CREATE FUNCTION doom_tic_weapon(a_map_id INT, a_p INT, a_plan BIGINT) RETURNS BIGINT
BEGIN
  DECLARE v_out BIGINT;
  DECLARE v_shot_serial BIGINT;
  DECLARE v_impacts INT;
  SET v_out = a_plan;
  IF (a_plan & 16) <> 0 THEN
    SET v_shot_serial = doom_cs_weapon(a_map_id,a_p);
    SET v_out = doom_cs_plan(a_map_id,a_p);
    IF (v_out & 32) <> 0 THEN
      SET v_impacts = doom_cs_hitscan_fire(a_map_id,a_p);
      IF v_impacts >= 0 THEN
        DO doom_cs_hitscan_apply(a_map_id,a_p,v_shot_serial);
      END IF;
    END IF;
  END IF;
  RETURN v_out;
END;;

-- Deathmatch, per player: the PLAY frame the other clients draw this player
-- with, and P_DeathThink's respawn -- a second on the floor, then any fire or
-- use press.
DROP FUNCTION IF EXISTS doom_tic_dm_player;;
CREATE FUNCTION doom_tic_dm_player(a_map_id INT, a_p INT) RETURNS INT
BEGIN
  DECLARE v_rs BOOLEAN;
  DECLARE v_r BOOLEAN;
  DO doom_cs_player_pose(a_map_id,a_p);
  SET v_rs = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT (NOT ps.alive AND ps.death_tics > 35 AND (g.attack_held OR g.use_requested)) AS r
      FROM player_state ps
      JOIN game_tic_commands g ON g.map_id=ps.map_id AND g.player_thing_id=ps.player_thing_id
      WHERE ps.map_id=a_map_id AND ps.player_thing_id=a_p;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_r;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_rs = v_r;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_rs THEN
    DO doom_mp_respawn(a_map_id,a_p);
  END IF;
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_run_tic_core;;
CREATE FUNCTION doom_run_tic_core(a_map_id INT, a_skill INT, a_p1 INT, a_p2 INT, a_p3 INT, a_p4 INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_fired BIGINT;
  DECLARE v_deathmatch BOOLEAN;
  DECLARE v_has3 BOOLEAN;
  DECLARE v_has4 BOOLEAN;
  DECLARE v_plan1 BIGINT;
  DECLARE v_plan2 BIGINT;
  DECLARE v_plan3 BIGINT;
  DECLARE v_plan4 BIGINT;
  DECLARE v_plans BIGINT;
  DECLARE v_sound_due BOOLEAN;
  DECLARE v_use_queued BOOLEAN;
  DECLARE v_u2 BOOLEAN;
  DECLARE v_u3 BOOLEAN;
  DECLARE v_u4 BOOLEAN;
  DECLARE v_active BIGINT;
  DECLARE v_did1 INT;
  DECLARE v_d2 INT;
  DECLARE v_d3 INT;
  DECLARE v_d4 INT;
  DECLARE v_pm INT;
  DECLARE v_x BIGINT;
  -- Stage trace. Bits match dock.TIC_STAGES; see tic_trace in schema.sql.
  SET v_fired = 1::bigint;
  SET v_deathmatch = a_p2 IS NOT NULL;
  SET v_has3 = a_p3 IS NOT NULL;
  SET v_has4 = a_p4 IS NOT NULL;
  DO doom_cs_clock(a_map_id,a_p1);
  IF v_deathmatch THEN
    DO doom_cs_clock(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    DO doom_cs_clock(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    DO doom_cs_clock(a_map_id,a_p4);
  END IF;
  SET v_fired = v_fired | 2::bigint;
  SET v_plan1 = doom_cs_plan(a_map_id,a_p1);
  SET v_plan2 = 0::bigint;
  SET v_plan3 = 0::bigint;
  SET v_plan4 = 0::bigint;
  IF v_deathmatch THEN
    SET v_plan2 = doom_cs_plan(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    SET v_plan3 = doom_cs_plan(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    SET v_plan4 = doom_cs_plan(a_map_id,a_p4);
  END IF;
  SET v_fired = v_fired | 4::bigint;
  SET v_plans = v_plan1 | v_plan2 | v_plan3 | v_plan4;
  SET v_sound_due = (v_plans & 128) <> 0;
  SET v_use_queued = doom_tic_use(a_map_id,a_p1,v_plan1);
  IF v_deathmatch THEN
    SET v_u2 = doom_tic_use(a_map_id,a_p2,v_plan2);
    SET v_use_queued = v_use_queued OR v_u2;
  END IF;
  IF v_has3 THEN
    SET v_u3 = doom_tic_use(a_map_id,a_p3,v_plan3);
    SET v_use_queued = v_use_queued OR v_u3;
  END IF;
  IF v_has4 THEN
    SET v_u4 = doom_tic_use(a_map_id,a_p4,v_plan4);
    SET v_use_queued = v_use_queued OR v_u4;
  END IF;
  IF (v_plans & 1) <> 0 THEN
    SET v_fired = v_fired | 8::bigint;
  END IF;
  SET v_active = 0::bigint;
  IF (v_plans & 2) <> 0 OR v_use_queued THEN
    SET v_active = doom_cs_activate_specials(a_map_id);
    SET v_fired = v_fired | 16::bigint;
  END IF;
  IF (v_plans & 4) <> 0 OR v_active <> 0 THEN
    DO doom_cs_doors(a_map_id,a_p1);
    SET v_fired = v_fired | 32::bigint;
  END IF;
  -- The plan once carried a scroller bit here. Special-48 wall scrolling is a
  -- function of level_tics that the renderer applies per frame, so there was
  -- nothing to step, and the bit is gone rather than left as a hole.
  SET v_did1 = doom_tic_move(a_map_id,a_p1);
  IF v_did1 = 1 THEN
    SET v_fired = v_fired | 128::bigint;
  END IF;
  IF v_did1 = 2 THEN
    SET v_fired = v_fired | 256::bigint;
  END IF;
  IF v_deathmatch THEN
    SET v_d2 = doom_tic_move(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    SET v_d3 = doom_tic_move(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    SET v_d4 = doom_tic_move(a_map_id,a_p4);
  END IF;
  -- After the movement stage, so nothing writes the standing eye height back
  -- over the sinking one. A no-op while the player is alive.
  DO doom_cs_death(a_map_id,a_p1);
  IF v_deathmatch THEN
    DO doom_cs_death(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    DO doom_cs_death(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    DO doom_cs_death(a_map_id,a_p4);
  END IF;
  SET v_plan1 = doom_cs_plan(a_map_id,a_p1);
  IF v_deathmatch THEN
    SET v_plan2 = doom_cs_plan(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    SET v_plan3 = doom_cs_plan(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    SET v_plan4 = doom_cs_plan(a_map_id,a_p4);
  END IF;
  SET v_plans = v_plan1 | v_plan2 | v_plan3 | v_plan4;
  SET v_sound_due = v_sound_due OR (v_plans & 128) <> 0;
  IF (v_plans & 8) <> 0 THEN
    SET v_fired = v_fired | 512::bigint | 1024::bigint | 2048::bigint;
  END IF;
  SET v_plan1 = doom_tic_secrets(a_map_id,a_p1,v_plan1);
  IF v_deathmatch THEN
    SET v_plan2 = doom_tic_secrets(a_map_id,a_p2,v_plan2);
  END IF;
  IF v_has3 THEN
    SET v_plan3 = doom_tic_secrets(a_map_id,a_p3,v_plan3);
  END IF;
  IF v_has4 THEN
    SET v_plan4 = doom_tic_secrets(a_map_id,a_p4,v_plan4);
  END IF;
  SET v_plans = v_plan1 | v_plan2 | v_plan3 | v_plan4;
  SET v_sound_due = v_sound_due OR (v_plans & 128) <> 0;
  IF (v_plans & 16) <> 0 THEN
    SET v_fired = v_fired | 4096::bigint;
  END IF;
  SET v_plan1 = doom_tic_weapon(a_map_id,a_p1,v_plan1);
  IF v_deathmatch THEN
    SET v_plan2 = doom_tic_weapon(a_map_id,a_p2,v_plan2);
  END IF;
  IF v_has3 THEN
    SET v_plan3 = doom_tic_weapon(a_map_id,a_p3,v_plan3);
  END IF;
  IF v_has4 THEN
    SET v_plan4 = doom_tic_weapon(a_map_id,a_p4,v_plan4);
  END IF;
  SET v_plans = v_plan1 | v_plan2 | v_plan3 | v_plan4;
  SET v_sound_due = v_sound_due OR (v_plans & 128) <> 0;
  IF (v_plans & 32) <> 0 THEN
    SET v_fired = v_fired | 8192::bigint | 16384::bigint;
  END IF;
  -- World stages once. The projectile stage spawns every player's missiles and
  -- tests every player as a target; the sound stage runs per player because its
  -- player cues are keyed by player, while its world cues dedupe on their keys.
  -- The player the world stages see: in a deathmatch a different live, held
  -- slot every tic, so monsters notice everyone (doom_cs_monsters keeps the
  -- target a monster picked; this only decides who is offered).
  SET v_pm = a_p1;
  IF v_deathmatch THEN
    BEGIN
      DECLARE b1_done INT DEFAULT 0;
      DECLARE b1_rows CURSOR FOR SELECT q.player_thing_id AS x
          FROM (SELECT mp.player_thing_id, ps.level_tics,
                       ROW_NUMBER() OVER (ORDER BY mp.slot) AS rn, COUNT(*) OVER () AS n
                FROM mp_players mp
                JOIN player_state ps ON ps.map_id=mp.map_id AND ps.player_thing_id=mp.player_thing_id
                WHERE mp.map_id=a_map_id AND mp.role_name IS NOT NULL AND ps.alive) q
          WHERE q.rn = (q.level_tics % q.n) + 1;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
      OPEN b1_rows;
      b1_each: LOOP
        FETCH b1_rows INTO v_x;
        IF b1_done THEN LEAVE b1_each; END IF;
        SET v_pm = v_x;
      END LOOP;
      CLOSE b1_rows;
    END;
  END IF;
  IF (v_plans & 64) <> 0 THEN
    DO doom_cs_projectiles(a_map_id,v_pm);
    SET v_fired = v_fired | 32768::bigint;
    SET v_plan1 = doom_cs_plan(a_map_id,a_p1);
    IF v_deathmatch THEN
      SET v_plan2 = doom_cs_plan(a_map_id,a_p2);
    END IF;
    IF v_has3 THEN
      SET v_plan3 = doom_cs_plan(a_map_id,a_p3);
    END IF;
    IF v_has4 THEN
      SET v_plan4 = doom_cs_plan(a_map_id,a_p4);
    END IF;
    SET v_sound_due = v_sound_due OR ((v_plan1 | v_plan2 | v_plan3 | v_plan4) & 128) <> 0;
  END IF;
  IF v_sound_due THEN
    DO doom_cs_sound(a_map_id,a_p1);
    IF v_deathmatch THEN
      DO doom_cs_sound(a_map_id,a_p2);
    END IF;
    IF v_has3 THEN
      DO doom_cs_sound(a_map_id,a_p3);
    END IF;
    IF v_has4 THEN
      DO doom_cs_sound(a_map_id,a_p4);
    END IF;
    SET v_fired = v_fired | 65536::bigint;
  ELSE
    UPDATE monster_ai SET fired_this_tick=FALSE WHERE map_id=a_map_id AND fired_this_tick;
  END IF;
  DO doom_cs_monsters(a_map_id,v_pm);
  SET v_fired = v_fired | 131072::bigint;
  IF v_deathmatch THEN
    DO doom_cs_item_respawn(a_map_id,a_p1);
  END IF;
  -- Animated sector lighting and damaging floors. Unconditional like the
  -- monsters: it writes only the sectors whose value actually changed, so an
  -- idle tic costs a comparison per animated sector and nothing else.
  DO doom_cs_sector_fx(a_map_id,a_p1);
  IF v_deathmatch THEN
    DO doom_cs_sector_fx(a_map_id,a_p2);
  END IF;
  IF v_has3 THEN
    DO doom_cs_sector_fx(a_map_id,a_p3);
  END IF;
  IF v_has4 THEN
    DO doom_cs_sector_fx(a_map_id,a_p4);
  END IF;
  SET v_fired = v_fired | 262144::bigint;
  -- P_XYMovement for Things, after the monsters have handed out their damage so
  -- a shove given this tic is carried this tic. Reads nothing when nothing is
  -- moving, which is almost always.
  DO doom_cs_thing_physics(a_map_id);
  -- A_BossDeath, after the monsters have taken their damage for this tic so a
  -- boss that died in it counts immediately. Cheap on every ordinary map: the
  -- aggregate it runs has no group to work on unless the map is in
  -- boss_actions.
  DO doom_cs_boss(a_map_id);
  SET v_fired = v_fired | 524288::bigint;
  IF v_deathmatch THEN
    -- Scoreboard once, then each player's pose and respawn.
    DO doom_mp_frags(a_map_id);
    DO doom_tic_dm_player(a_map_id,a_p1);
    DO doom_tic_dm_player(a_map_id,a_p2);
    IF v_has3 THEN
      DO doom_tic_dm_player(a_map_id,a_p3);
    END IF;
    IF v_has4 THEN
      DO doom_tic_dm_player(a_map_id,a_p4);
    END IF;
  END IF;
  -- Most consecutive tics run exactly the same set of stages, and the client
  -- re-reads this row every tic regardless, so skipping the unchanged writes
  -- costs it nothing and saves a row version per tic.
  INSERT INTO tic_trace (map_id,player_thing_id,stages)
  VALUES (a_map_id::int,a_p1::int,v_fired)
  ON CONFLICT (map_id,player_thing_id) DO UPDATE SET stages=EXCLUDED.stages
  WHERE tic_trace.stages <> EXCLUDED.stages;
  RETURN v_sound_due;
END;;

-- Single player: the client's command straight in, then the shared tic.
DROP FUNCTION IF EXISTS doom_run_game_tic;;
CREATE FUNCTION doom_run_game_tic(a_map_id INT, a_player_thing_id INT, a_skill INT, a_move_fwd DOUBLE, a_move_strafe DOUBLE, a_running BOOLEAN, a_turn_degrees DOUBLE, a_attack_held BOOLEAN, a_weapon_switch_to INT, a_use_requested BOOLEAN, a_jump_requested BOOLEAN) RETURNS BOOLEAN
BEGIN
  DECLARE v_playing INT;
  DECLARE v_recording INT;
  DECLARE v_dtic BIGINT;
  DECLARE v_demo_playing BIGINT;
  DECLARE v_demo_recording BIGINT;
  DECLARE v_demo_tic BIGINT;
  DECLARE v_have BOOLEAN;
  DECLARE v_e BOOLEAN;
  DECLARE v_nobody INT;
  DO doom_cs_begin(
    a_map_id,a_player_thing_id,a_skill,a_move_fwd,a_move_strafe,
    a_running,a_turn_degrees,a_attack_held,a_weapon_switch_to,a_use_requested,a_jump_requested);
  -- G_ReadDemoTiccmd / G_WriteDemoTiccmd: while a demo plays, its recorded
  -- command replaces the client's; while one records, the command is appended.
  -- Past the last recorded tic the demo ends, and if the attract loop started
  -- it the title comes back for the loop's next page.
  SET v_playing = NULL::int;
  SET v_recording = NULL::int;
  SET v_dtic = 0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT demo_playing, demo_recording, demo_tic FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_demo_playing, v_demo_recording, v_demo_tic;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_playing = v_demo_playing;
      SET v_recording = v_demo_recording;
      SET v_dtic = v_demo_tic;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_playing IS NOT NULL THEN
    SET v_have = false;
    BEGIN
      DECLARE b2_done INT DEFAULT 0;
      DECLARE b2_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM demo_tics d WHERE d.demo_id=v_playing AND d.tic=v_dtic) AS e;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
      OPEN b2_rows;
      b2_each: LOOP
        FETCH b2_rows INTO v_e;
        IF b2_done THEN LEAVE b2_each; END IF;
        SET v_have = v_e;
      END LOOP;
      CLOSE b2_rows;
    END;
    IF v_have THEN
      UPDATE game_tic_commands g
          SET move_fwd=d.move_fwd, move_strafe=d.move_strafe, running=d.running,
              turn_degrees=d.turn_degrees, attack_held=d.attack_held,
              weapon_switch_to=d.weapon_switch_to, use_requested=d.use_requested
          FROM demo_tics d
          WHERE d.demo_id=v_playing AND d.tic=v_dtic
            AND g.map_id=a_map_id AND g.player_thing_id=a_player_thing_id;
      UPDATE screen_state SET demo_tic=demo_tic+1 WHERE id=0;
    END IF;
    IF NOT v_have THEN
      UPDATE screen_state
          SET demo_playing=NULL, demo_tic=0,
              screen=CASE WHEN attract_step>=0 THEN 'title'::screen_kind ELSE screen END,
              attract_pagetic=0
          WHERE id=0;
    END IF;
  END IF;
  IF v_recording IS NOT NULL THEN
    INSERT INTO demo_tics
        (demo_id,tic,move_fwd,move_strafe,running,turn_degrees,attack_held,
         weapon_switch_to,use_requested)
      SELECT v_recording,v_dtic,g.move_fwd,g.move_strafe,g.running,g.turn_degrees,
             g.attack_held,g.weapon_switch_to,g.use_requested
      FROM game_tic_commands g
      WHERE g.map_id=a_map_id AND g.player_thing_id=a_player_thing_id
      ON CONFLICT (demo_id,tic) DO UPDATE SET
        move_fwd=EXCLUDED.move_fwd, move_strafe=EXCLUDED.move_strafe,
        running=EXCLUDED.running, turn_degrees=EXCLUDED.turn_degrees,
        attack_held=EXCLUDED.attack_held,
        weapon_switch_to=EXCLUDED.weapon_switch_to,
        use_requested=EXCLUDED.use_requested;
    UPDATE demos SET tic_count=GREATEST(tic_count,v_dtic+1) WHERE demo_id=v_recording;
    UPDATE screen_state SET demo_tic=demo_tic+1 WHERE id=0;
  END IF;
  SET v_nobody = NULL::int;
  RETURN doom_run_tic_core(a_map_id,a_skill,a_player_thing_id,v_nobody,v_nobody,v_nobody);
END;;

-- The input rows one client appended since the last tic, folded into its
-- command: the latest movement values, the summed turn, any press.
DROP FUNCTION IF EXISTS doom_mp_begin_player;;
CREATE FUNCTION doom_mp_begin_player(a_map_id INT, a_skill INT, a_p INT, a_upto BIGINT) RETURNS INT
BEGIN
  DECLARE v_mf DOUBLE;
  DECLARE v_ms DOUBLE;
  DECLARE v_mr BOOLEAN;
  DECLARE v_mt DOUBLE;
  DECLARE v_ma BOOLEAN;
  DECLARE v_mw INT;
  DECLARE v_mu BOOLEAN;
  DECLARE v_f DOUBLE;
  DECLARE v_s DOUBLE;
  DECLARE v_r DOUBLE;
  DECLARE v_t DOUBLE;
  DECLARE v_a BOOLEAN;
  DECLARE v_w INT;
  DECLARE v_u BOOLEAN;
  SET v_mf = 0::real;
  SET v_ms = 0::real;
  SET v_mr = false;
  SET v_mt = 0::real;
  SET v_ma = false;
  SET v_mw = NULL::int;
  SET v_mu = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(ARG_MAX(move_fwd, seq), 0)::real AS f,
             COALESCE(ARG_MAX(move_strafe, seq), 0)::real AS s,
             COALESCE(ARG_MAX(running, seq), FALSE) AS r,
             COALESCE(SUM(turn_degrees), 0)::real AS t,
             COALESCE(BOOL_OR(attack_held), FALSE) AS a,
             MAX(weapon_switch_to)::int AS w,
             COALESCE(BOOL_OR(use_requested), FALSE) AS u
      FROM mp_inputs WHERE map_id=a_map_id AND player_thing_id=a_p AND seq <= a_upto;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_f, v_s, v_r, v_t, v_a, v_w, v_u;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_mf = v_f;
      SET v_ms = v_s;
      SET v_mr = v_r;
      SET v_mt = v_t;
      SET v_ma = v_a;
      SET v_mw = v_w;
      SET v_mu = v_u;
    END LOOP;
    CLOSE b1_rows;
  END;
  DO doom_cs_begin(a_map_id,a_p,a_skill,v_mf,v_ms,v_mr,v_mt,v_ma,v_mw,v_mu,FALSE);
  RETURN 1;
END;;

-- A slot claimed since the last tic (api_join): a fresh body and a clean
-- score. Only this player's request is deleted: a join for another slot
-- landing at this very moment must not be touched, or one of the two fails
-- to serialize.
DROP FUNCTION IF EXISTS doom_mp_spawn_if_requested;;
CREATE FUNCTION doom_mp_spawn_if_requested(a_map_id INT, a_p INT) RETURNS INT
BEGIN
  DECLARE v_spawn BOOLEAN;
  DECLARE v_e BOOLEAN;
  SET v_spawn = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS(SELECT 1 FROM mp_join_requests r JOIN mp_players mp
                    ON mp.map_id=r.map_id AND mp.slot=r.slot
                    WHERE mp.map_id=a_map_id AND mp.player_thing_id=a_p) AS e;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_spawn = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_spawn THEN
    UPDATE player_state SET frags=0 WHERE map_id=a_map_id AND player_thing_id=a_p;
    DO doom_mp_respawn(a_map_id,a_p);
    DELETE FROM mp_join_requests WHERE map_id=a_map_id
        AND slot IN (SELECT mp.slot FROM mp_players mp
                     WHERE mp.map_id=a_map_id AND mp.player_thing_id=a_p);
  END IF;
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_run_mp_tic;;
CREATE FUNCTION doom_run_mp_tic(a_map_id INT, a_skill INT, a_p1 INT, a_p2 INT, a_p3 INT, a_p4 INT) RETURNS BOOLEAN
BEGIN
  DECLARE v_upto BIGINT;
  DECLARE v_m BIGINT;
  -- Consume every input row that exists now; rows a client appends during the
  -- tic are next tic's.
  SET v_upto = 0::bigint;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(MAX(seq),0)::bigint AS m FROM mp_inputs WHERE map_id=a_map_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_m;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_upto = v_m;
    END LOOP;
    CLOSE b1_rows;
  END;
  DO doom_mp_spawn_if_requested(a_map_id,a_p1);
  IF a_p2 IS NOT NULL THEN
    DO doom_mp_spawn_if_requested(a_map_id,a_p2);
  END IF;
  IF a_p3 IS NOT NULL THEN
    DO doom_mp_spawn_if_requested(a_map_id,a_p3);
  END IF;
  IF a_p4 IS NOT NULL THEN
    DO doom_mp_spawn_if_requested(a_map_id,a_p4);
  END IF;
  DO doom_mp_begin_player(a_map_id,a_skill,a_p1,v_upto);
  IF a_p2 IS NOT NULL THEN
    DO doom_mp_begin_player(a_map_id,a_skill,a_p2,v_upto);
  END IF;
  IF a_p3 IS NOT NULL THEN
    DO doom_mp_begin_player(a_map_id,a_skill,a_p3,v_upto);
  END IF;
  IF a_p4 IS NOT NULL THEN
    DO doom_mp_begin_player(a_map_id,a_skill,a_p4,v_upto);
  END IF;
  DELETE FROM mp_inputs WHERE map_id=a_map_id AND seq <= v_upto;
  DO doom_run_tic_core(a_map_id,a_skill,a_p1,a_p2,a_p3,a_p4);
  -- TRUE when this tic ended the level: the referee then runs the intermission
  -- and moves the match on (doom_mp_rotate).
  RETURN doom_mp_level_done(a_map_id);
END;;

-- ==== 41_flow.sql ====
-- ---------------------------------------------------------------------------
-- G_RecordDemo: start recording under a name, replacing a demo of that name.
DROP FUNCTION IF EXISTS doom_demo_begin;;
CREATE FUNCTION doom_demo_begin(a_name TEXT, a_map_id INT, a_player_thing_id INT, a_skill INT) RETURNS INT
BEGIN
  DECLARE v_did BIGINT;
  DECLARE v_i BOOLEAN;
  SET v_did = 0;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE((SELECT d.demo_id FROM demos d WHERE d.name=a_name),
                      (SELECT COALESCE(MAX(x.demo_id),0)+1 FROM demos x)) AS i;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_i;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_did = v_i;
    END LOOP;
    CLOSE b1_rows;
  END;
  DELETE FROM demo_tics WHERE demo_id=v_did;
  INSERT INTO demos (demo_id,name,map_id,player_thing_id,skill,tic_count)
  VALUES (v_did,a_name,a_map_id,a_player_thing_id,a_skill,0)
  ON CONFLICT (demo_id) DO UPDATE
  SET name=EXCLUDED.name, map_id=EXCLUDED.map_id,
      player_thing_id=EXCLUDED.player_thing_id, skill=EXCLUDED.skill,
      tic_count=0, recorded_at=NOW();
  UPDATE screen_state SET demo_recording=v_did, demo_playing=NULL, demo_tic=0 WHERE id=0;
  RETURN v_did;
END;;

-- G_DeferedPlayDemo: play the named demo from its first tic. -1 if unknown.
DROP FUNCTION IF EXISTS doom_demo_play;;
CREATE FUNCTION doom_demo_play(a_name TEXT) RETURNS INT
BEGIN
  DECLARE v_did BIGINT;
  DECLARE v_i BIGINT;
  SET v_did = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT d.demo_id AS i FROM demos d WHERE d.name=a_name;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_i;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_did = v_i;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_did >= 0 THEN
    UPDATE screen_state SET demo_playing=v_did, demo_recording=NULL, demo_tic=0 WHERE id=0;
  END IF;
  RETURN v_did;
END;;

DROP FUNCTION IF EXISTS doom_demo_stop;;
CREATE FUNCTION doom_demo_stop() RETURNS INT
BEGIN
  UPDATE screen_state SET demo_playing=NULL, demo_recording=NULL, demo_tic=0 WHERE id=0;
  RETURN 0;
END;;

-- ---------------------------------------------------------------------------
-- D_DoAdvanceDemo. Vanilla alternates a page and a demo -- title, demo1,
-- credit, demo2, credit, demo3, demo4 -- holding the title for 170 tics and
-- the other pages for 200, and wraps. Here the pages alternate the same way
-- and the demos are whatever is seeded, so a build with one demo still cycles
-- title, demo, credit, demo rather than stalling.
DROP FUNCTION IF EXISTS doom_attract_start;;
CREATE FUNCTION doom_attract_start() RETURNS INT
BEGIN
  -- A client that died mid-demo leaves demo_playing set; a fresh start owns it.
  UPDATE screen_state
  SET attract_step=0, attract_pagetic=170, screen='title', cursor_index=0,
      demo_playing=NULL, demo_recording=NULL, demo_tic=0
  WHERE id=0;
  RETURN 0;
END;;

-- G_Responder: a key during the attract loop ends it. A demo that was playing
-- stops where it is and the title comes back.
DROP FUNCTION IF EXISTS doom_attract_stop;;
CREATE FUNCTION doom_attract_stop() RETURNS INT
BEGIN
  UPDATE screen_state
  SET attract_step=-1,
      screen=CASE WHEN demo_playing IS NOT NULL THEN 'title'::screen_kind ELSE screen END,
      demo_playing=NULL, demo_tic=0
  WHERE id=0;
  RETURN 0;
END;;

-- One 35 Hz step of the loop while a page is up. Returns 'none', 'page' (the
-- screen changed) or 'demo:<id>' (start that demo on its own map and skill).
DROP FUNCTION IF EXISTS doom_attract_tic;;
CREATE FUNCTION doom_attract_tic() RETURNS TEXT
BEGIN
  DECLARE v_step BIGINT;
  DECLARE v_pagetic DOUBLE;
  DECLARE v_playing INT;
  DECLARE v_attract_step BIGINT;
  DECLARE v_attract_pagetic DOUBLE;
  DECLARE v_demo_playing BIGINT;
  DECLARE v_n BIGINT;
  DECLARE v_c INT;
  DECLARE v_slot BIGINT;
  DECLARE v_did BIGINT;
  DECLARE v_i BIGINT;
  SET v_step = -1;
  SET v_pagetic = 0::real;
  SET v_playing = NULL::int;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT attract_step, attract_pagetic, demo_playing FROM screen_state WHERE id=0;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_attract_step, v_attract_pagetic, v_demo_playing;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_step = v_attract_step;
      SET v_pagetic = v_attract_pagetic;
      SET v_playing = v_demo_playing;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_step < 0 OR v_playing IS NOT NULL THEN
    RETURN 'none';
  END IF;
  IF v_pagetic > 1 THEN
    UPDATE screen_state SET attract_pagetic = attract_pagetic - 1 WHERE id=0;
    RETURN 'none';
  END IF;
  SET v_step = v_step + 1;
  SET v_n = 0;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT count(*)::int AS c FROM demos;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_c;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_n = v_c;
    END LOOP;
    CLOSE b2_rows;
  END;
  IF v_n = 0 THEN
    UPDATE screen_state SET attract_step=v_step, attract_pagetic=170, screen='title', cursor_index=0 WHERE id=0;
    RETURN 'page';
  END IF;
  SET v_slot = v_step % (2*v_n);
  IF v_slot % 2 = 0 THEN
    UPDATE screen_state
      SET attract_step=v_step,
          attract_pagetic=CASE WHEN v_slot=0 THEN 170 ELSE 200 END,
          screen=CASE WHEN v_slot=0 THEN 'title' ELSE 'help2' END, cursor_index=0
      WHERE id=0;
    RETURN 'page';
  END IF;
  SET v_did = -1;
  BEGIN
    DECLARE b3_done INT DEFAULT 0;
    DECLARE b3_rows CURSOR FOR SELECT q.demo_id AS i
      FROM (SELECT d.demo_id, ROW_NUMBER() OVER (ORDER BY d.demo_id) AS rn FROM demos d) q
      WHERE q.rn = v_slot/2 + 1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
    OPEN b3_rows;
    b3_each: LOOP
      FETCH b3_rows INTO v_i;
      IF b3_done THEN LEAVE b3_each; END IF;
      SET v_did = v_i;
    END LOOP;
    CLOSE b3_rows;
  END;
  UPDATE screen_state
  SET attract_step=v_step, attract_pagetic=0, demo_playing=v_did, demo_recording=NULL,
      demo_tic=0, screen='game'
  WHERE id=0;
  RETURN 'demo:' || v_did::text;
END;;

-- ---------------------------------------------------------------------------
-- G_DoCompleted: freeze the level's totals, compute the tally and -- following
-- Doom's routing -- which level comes next. Returns its map_id, or -1 when
-- the episode ends here or that map is not loaded.
DROP FUNCTION IF EXISTS doom_level_exit;;
CREATE FUNCTION doom_level_exit(a_map_id INT, a_player_thing_id INT, a_secret_exit BOOLEAN) RETURNS INT
BEGIN
  DO doom_finish_level(a_map_id,a_player_thing_id,a_secret_exit);
  RETURN doom_intermission_begin(a_map_id,a_player_thing_id,a_secret_exit);
END;;

-- G_WorldDone, once the tally has been dismissed: 'level:<map_id>' to enter
-- next, or 'finale' when the episode is over and has its text to show. An
-- episode with no finale seeded wraps to the first map so a demo keeps going.
DROP FUNCTION IF EXISTS doom_after_intermission;;
CREATE FUNCTION doom_after_intermission(a_map_id INT) RETURNS TEXT
BEGIN
  DECLARE v_nxt BIGINT;
  DECLARE v_ep BIGINT;
  DECLARE v_n BIGINT;
  DECLARE v_e INT;
  DECLARE v_found INT;
  DECLARE v_first BIGINT;
  DECLARE v_f INT;
  SET v_nxt = -1;
  SET v_ep = 1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT COALESCE(s.next_map_id,-1) AS n, substring(m.name FROM 2 FOR 1)::int AS e
      FROM screen_state s, maps m WHERE s.id=0 AND m.map_id=a_map_id;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_n, v_e;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_nxt = v_n;
      SET v_ep = v_e;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_nxt >= 0 THEN
    RETURN 'level:' || v_nxt::text;
  END IF;
  SET v_found = doom_finale_begin(v_ep);
  IF v_found <> 0 THEN
    RETURN 'finale';
  END IF;
  SET v_first = 1;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT MIN(map_id)::int AS f FROM maps;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_f;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_first = v_f;
    END LOOP;
    CLOSE b2_rows;
  END;
  RETURN 'level:' || v_first::text;
END;;

-- G_DoLoadLevel: the world reset, the player on their start, the level's
-- secret count recorded, and -- crossing from a finished level -- the state
-- the player actually ended it with put back over Doom's reborn loadout.
DROP FUNCTION IF EXISTS doom_enter_level;;
CREATE FUNCTION doom_enter_level(a_map_id INT, a_player_thing_id INT, a_skill INT, a_from_map_id INT, a_from_player_thing_id INT) RETURNS INT
BEGIN
  DO doom_reset_stage(a_map_id,a_skill);
  DO doom_spawn_player(a_map_id,a_player_thing_id);
  DO doom_cs_secret(a_map_id,a_player_thing_id);
  IF a_from_map_id IS NOT NULL THEN
    DO doom_carry_player(a_from_map_id,a_from_player_thing_id,a_map_id,a_player_thing_id);
  END IF;
  UPDATE screen_state SET cheat_buffer='' WHERE id=0;
  RETURN 1;
END;;

-- ==== 42_api.sql ====
-- Join the match
DROP FUNCTION IF EXISTS api_join;;
CREATE FUNCTION api_join() RETURNS INT
BEGIN
  DECLARE v_spectator BOOLEAN;
  DECLARE v_x BOOLEAN;
  DECLARE v_pid BIGINT;
  DECLARE v_p BIGINT;
  DECLARE v_playing BOOLEAN;
  DECLARE v_x_2 BOOLEAN;
  DECLARE v_claim BIGINT;
  DECLARE v_claim_map BIGINT;
  DECLARE v_claim_slot BIGINT;
  DECLARE v_first BOOLEAN;
  DECLARE v_x_3 BOOLEAN;
  DECLARE v_m BIGINT;
  DECLARE v_s BIGINT;
  DECLARE v_p_2 BIGINT;
  SET v_spectator = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT EXISTS (SELECT 1 FROM api_spectator_roles s WHERE s.role_name = session_user::text) AS x;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_spectator = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_spectator THEN
    RETURN -1;
  END IF;
  SET v_pid = -1;
  BEGIN
    DECLARE b2_done INT DEFAULT 0;
    DECLARE b2_rows CURSOR FOR SELECT mp.player_thing_id AS p FROM mp_players mp
      WHERE mp.role_name = session_user::text;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b2_done = 1;
    OPEN b2_rows;
    b2_each: LOOP
      FETCH b2_rows INTO v_p;
      IF b2_done THEN LEAVE b2_each; END IF;
      SET v_pid = v_p;
    END LOOP;
    CLOSE b2_rows;
  END;
  IF v_pid < 0 THEN
    -- The queue: a caller without a slot is a waiter from its first call, and
    -- stays one while it keeps calling (the page asks once a second). Waiters
    -- quiet for five seconds have left. First come, first served: a free slot
    -- goes to the earliest waiter only.
    INSERT INTO mp_waiting (role_name, since, last_seen) VALUES (session_user::text, now(), now())
      ON CONFLICT (role_name) DO UPDATE SET last_seen = now();
  END IF;
  -- Between maps nothing is claimed or written to the slot table: the referee
  -- is moving the slots to the next map and a concurrent write would only make
  -- it retry. The client asks again in a second; api_match says what is going on.
  SET v_playing = false;
  BEGIN
    DECLARE b3_done INT DEFAULT 0;
    DECLARE b3_rows CURSOR FOR SELECT (m.state = 'playing') AS x FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b3_done = 1;
    OPEN b3_rows;
    b3_each: LOOP
      FETCH b3_rows INTO v_x_2;
      IF b3_done THEN LEAVE b3_each; END IF;
      SET v_playing = v_x_2;
    END LOOP;
    CLOSE b3_rows;
  END;
  IF NOT v_playing THEN
    RETURN -1;
  END IF;
  SET v_claim = -1;
  SET v_claim_map = 0;
  SET v_claim_slot = 0;
  SET v_first = false;
  BEGIN
    DECLARE b4_done INT DEFAULT 0;
    DECLARE b4_rows CURSOR FOR SELECT NOT EXISTS (SELECT 1 FROM mp_waiting w, mp_waiting me
                         WHERE me.role_name = session_user::text AND w.since < me.since) AS x;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b4_done = 1;
    OPEN b4_rows;
    b4_each: LOOP
      FETCH b4_rows INTO v_x_3;
      IF b4_done THEN LEAVE b4_each; END IF;
      SET v_first = v_x_3;
    END LOOP;
    CLOSE b4_rows;
  END;
  IF v_pid < 0 AND v_first THEN
    BEGIN
      DECLARE b5_done INT DEFAULT 0;
      DECLARE b5_rows CURSOR FOR SELECT mp.map_id AS m, mp.slot AS s, mp.player_thing_id AS p FROM mp_players mp
          WHERE mp.role_name IS NULL ORDER BY mp.map_id, mp.slot LIMIT 1;
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET b5_done = 1;
      OPEN b5_rows;
      b5_each: LOOP
        FETCH b5_rows INTO v_m, v_s, v_p_2;
        IF b5_done THEN LEAVE b5_each; END IF;
        SET v_claim_map = v_m;
        SET v_claim_slot = v_s;
        SET v_claim = v_p_2;
      END LOOP;
      CLOSE b5_rows;
    END;
  END IF;
  IF v_claim >= 0 THEN
    UPDATE mp_players SET role_name = session_user::text, claimed_at = now()
      WHERE map_id = v_claim_map AND slot = v_claim_slot AND role_name IS NULL;
    DELETE FROM mp_waiting WHERE role_name = session_user::text;
    INSERT INTO mp_join_requests (map_id, slot) VALUES (v_claim_map, v_claim_slot)
      ON CONFLICT DO NOTHING;
    SET v_pid = v_claim;
  END IF;
  IF v_pid >= 0 THEN
    UPDATE mp_players SET last_seen = now() WHERE role_name = session_user::text;
  END IF;
  IF v_claim >= 0 THEN
    -- A fresh claim gets a grace period before the reaper may take the slot
    -- back: the client compiles its frame statement next, which takes several
    -- seconds when several players join at once and would otherwise outlast
    -- IDLE_SECONDS. The heartbeats never shorten it (api_camera), so it is
    -- also how long a slot stays held after someone joins and leaves at once;
    -- 15 s covers the compile and keeps that wait short for the next in line.
    UPDATE mp_players SET last_seen = now() + interval '15 seconds'
      WHERE role_name = session_user::text;
  END IF;
  RETURN v_pid;
END;;

-- Which slot the caller holds (1 to 4), -1 for none. The frame views are per
-- slot: api_frame_slot<n> and api_frame_idx_slot<n>.
DROP FUNCTION IF EXISTS api_slot;;
CREATE FUNCTION api_slot() RETURNS INT
BEGIN
  DECLARE v_s BIGINT;
  DECLARE v_n BIGINT;
  SET v_s = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT mp.slot AS n FROM mp_players mp
      WHERE mp.role_name = session_user::text;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_n;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_s = v_n;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_s;
END;;

-- One frame of input, appended for the caller's slot. Values are clamped to
-- what the keyboard can produce, so no client is faster than another.
DROP FUNCTION IF EXISTS api_input;;
CREATE FUNCTION api_input(a_fwd DOUBLE, a_strafe DOUBLE, a_run BOOLEAN, a_turn DOUBLE, a_fire BOOLEAN, a_weapon INT, a_use BOOLEAN) RETURNS INT
BEGIN
  DECLARE v_playing BOOLEAN;
  DECLARE v_x BOOLEAN;
  SET v_playing = false;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT (m.state = 'playing') AS x FROM mp_match m;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_x;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_playing = v_x;
    END LOOP;
    CLOSE b1_rows;
  END;
  IF v_playing THEN
    INSERT INTO mp_inputs
      (map_id,player_thing_id,move_fwd,move_strafe,running,turn_degrees,
       attack_held,weapon_switch_to,use_requested)
    SELECT mp.map_id, mp.player_thing_id,
           LEAST(1.0, GREATEST(-1.0, COALESCE(a_fwd, 0)))::real,
           LEAST(1.0, GREATEST(-1.0, COALESCE(a_strafe, 0)))::real,
           COALESCE(a_run, FALSE),
           LEAST(180.0, GREATEST(-180.0, COALESCE(a_turn, 0)))::real,
           COALESCE(a_fire, FALSE),
           CASE WHEN a_weapon BETWEEN 1 AND 8 THEN a_weapon END,
           COALESCE(a_use, FALSE)
    FROM mp_players mp WHERE mp.role_name = session_user::text;
  END IF;
  RETURN 1;
END;;

-- Number-key weapon selection and mouse-wheel cycling, over the caller's
-- own weapons (the same rules as sql/client/weapon_slot.sql and
-- weapon_cycle.sql). -1 when the slot holds nothing the caller owns.
DROP FUNCTION IF EXISTS api_weapon_slot;;
CREATE FUNCTION api_weapon_slot(a_slot INT) RETURNS INT
BEGIN
  DECLARE v_w BIGINT;
  DECLARE v_id BIGINT;
  SET v_w = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR SELECT wd.weapon_id AS id
      FROM mp_players mp
      JOIN player_weapon_owned o ON o.map_id=mp.map_id AND o.player_thing_id=mp.player_thing_id
      JOIN weapon_defs wd ON wd.weapon_id=o.weapon_id
      JOIN player_weapons pw ON pw.map_id=o.map_id AND pw.player_thing_id=o.player_thing_id
      WHERE mp.role_name = session_user::text AND wd.slot=a_slot
      ORDER BY CASE
        WHEN a_slot=1 AND pw.current_weapon=8 AND wd.weapon_id=1 THEN 0
        WHEN a_slot=1 AND pw.current_weapon<>8 AND wd.weapon_id=8 THEN 0
        WHEN wd.weapon_id=pw.current_weapon THEN 2 ELSE 1 END,
        wd.weapon_id DESC
      LIMIT 1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_id;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_w = v_id;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_w;
END;;

DROP FUNCTION IF EXISTS api_weapon_cycle;;
CREATE FUNCTION api_weapon_cycle(a_step INT) RETURNS INT
BEGIN
  DECLARE v_w BIGINT;
  DECLARE v_id BIGINT;
  SET v_w = -1;
  BEGIN
    DECLARE b1_done INT DEFAULT 0;
    DECLARE b1_rows CURSOR FOR WITH me AS (
        SELECT mp.map_id, mp.player_thing_id FROM mp_players mp
        WHERE mp.role_name = session_user::text
      ),
      owned AS (
        SELECT wd.weapon_id,
               ROW_NUMBER() OVER (ORDER BY wd.slot, wd.weapon_id) AS pos,
               COUNT(*) OVER () AS n
        FROM me JOIN player_weapon_owned o ON o.map_id=me.map_id AND o.player_thing_id=me.player_thing_id
        JOIN weapon_defs wd ON wd.weapon_id = o.weapon_id
      ),
      here AS (
        SELECT o.pos, o.n
        FROM owned o JOIN me ON TRUE
        JOIN player_weapons pw ON pw.map_id = me.map_id AND pw.player_thing_id = me.player_thing_id
         AND pw.current_weapon = o.weapon_id
      )
      SELECT o.weapon_id AS id
      FROM owned o CROSS JOIN here h
      WHERE o.pos = ((h.pos - 1 + a_step + h.n) % h.n) + 1;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET b1_done = 1;
    OPEN b1_rows;
    b1_each: LOOP
      FETCH b1_rows INTO v_id;
      IF b1_done THEN LEAVE b1_each; END IF;
      SET v_w = v_id;
    END LOOP;
    CLOSE b1_rows;
  END;
  RETURN v_w;
END;;

-- The name the scoreboard and the frag messages use for the caller: letters,
-- digits, space, - and _, at most 12, upper case like everything the HUD
-- font can draw. Empty clears it back to the role name.
DROP FUNCTION IF EXISTS api_set_name;;
CREATE FUNCTION api_set_name(a_name TEXT) RETURNS INT
BEGIN
  UPDATE mp_players
  SET display_name = NULLIF(TRIM(UPPER(LEFT(REGEXP_REPLACE(COALESCE(a_name, ''), '[^A-Za-z0-9 _-]', '', 'g'), 12))), '')
  WHERE role_name = session_user::text;
  RETURN 1;
END;;

-- ==== 43_render_light.sql ====
-- doom_light_index: in render_light.sql

-- doom_light_scale: in render_light.sql

-- doom_light_zdepth: in render_light.sql

-- ==== 44_automap_view.sql ====
-- A row per player, made on demand so no caller has to seed it.
DROP FUNCTION IF EXISTS doom_automap_touch;;
CREATE FUNCTION doom_automap_touch(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  INSERT INTO automap_view (map_id, player_thing_id, center_x, center_y)
  SELECT a_map_id, a_player_thing_id, ps.position_x, ps.position_y
  FROM player_state ps
  WHERE ps.map_id = a_map_id AND ps.player_thing_id = a_player_thing_id
  ON CONFLICT (map_id, player_thing_id) DO NOTHING;
  RETURN 1;
END;;

-- Panning drops follow mode, exactly as AM_Drawer does when you move the view.
DROP FUNCTION IF EXISTS doom_automap_pan;;
CREATE FUNCTION doom_automap_pan(a_map_id INT, a_player_thing_id INT, a_dx DOUBLE, a_dy DOUBLE) RETURNS INT
BEGIN
  DO doom_automap_touch(a_map_id, a_player_thing_id);
  UPDATE automap_view av
  SET center_x = CASE WHEN av.follow THEN ps.position_x ELSE av.center_x END + a_dx,
      center_y = CASE WHEN av.follow THEN ps.position_y ELSE av.center_y END + a_dy,
      follow = FALSE
  FROM player_state ps
  WHERE av.map_id = a_map_id AND av.player_thing_id = a_player_thing_id
    AND ps.map_id = av.map_id AND ps.player_thing_id = av.player_thing_id;
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS doom_automap_zoom;;
CREATE FUNCTION doom_automap_zoom(a_map_id INT, a_player_thing_id INT, a_factor DOUBLE) RETURNS INT
BEGIN
  DECLARE v_lo DOUBLE;
  DECLARE v_hi DOUBLE;
  SET v_lo = doom_const('AUTOMAP_MIN_ZOOM');
  SET v_hi = doom_const('AUTOMAP_MAX_ZOOM');
  DO doom_automap_touch(a_map_id, a_player_thing_id);
  UPDATE automap_view
  SET zoom = GREATEST(v_lo, LEAST(v_hi, zoom * a_factor))::real
  WHERE map_id = a_map_id AND player_thing_id = a_player_thing_id;
  RETURN 1;
END;;

-- 'follow', 'grid', or 'cheat' (which cycles 0 -> 1 -> 2 -> 0, as IDDT does).
-- Turning follow back on re-centres by construction: the centre is read from
-- the player while it is set.
DROP FUNCTION IF EXISTS doom_automap_toggle;;
CREATE FUNCTION doom_automap_toggle(a_map_id INT, a_player_thing_id INT, a_what TEXT) RETURNS INT
BEGIN
  DO doom_automap_touch(a_map_id, a_player_thing_id);
  UPDATE automap_view
  SET follow = CASE WHEN a_what = 'follow' THEN NOT follow ELSE follow END,
      grid   = CASE WHEN a_what = 'grid'   THEN NOT grid   ELSE grid   END,
      cheat  = CASE WHEN a_what = 'cheat'  THEN ((cheat + 1) % 3)::smallint ELSE cheat END
  WHERE map_id = a_map_id AND player_thing_id = a_player_thing_id;
  RETURN 1;
END;;

-- The whole level, centred and padded: automap_fit_view, which was the only
-- reason a client ever needed the map's line list.
DROP FUNCTION IF EXISTS doom_automap_fit;;
CREATE FUNCTION doom_automap_fit(a_map_id INT, a_player_thing_id INT) RETURNS INT
BEGIN
  DECLARE v_world_height DOUBLE;
  DECLARE v_lo DOUBLE;
  DECLARE v_hi DOUBLE;
  SET v_world_height = doom_const('AUTOMAP_WORLD_HEIGHT');
  SET v_lo = doom_const('AUTOMAP_MIN_ZOOM');
  SET v_hi = doom_const('AUTOMAP_MAX_ZOOM');
  DO doom_automap_touch(a_map_id, a_player_thing_id);
  UPDATE automap_view av
  SET center_x = b.cx::real, center_y = b.cy::real,
      zoom = GREATEST(v_lo, LEAST(v_hi, b.fit))::real,
      follow = FALSE
  FROM (
    SELECT (MIN(LEAST(v1.x,v2.x)) + MAX(GREATEST(v1.x,v2.x))) / 2.0 AS cx,
           (MIN(LEAST(v1.y,v2.y)) + MAX(GREATEST(v1.y,v2.y))) / 2.0 AS cy,
           LEAST((320.0 - 96.0) / GREATEST(1.0, MAX(GREATEST(v1.x,v2.x)) - MIN(LEAST(v1.x,v2.x))),
                 (200.0 - 144.0) / GREATEST(1.0, MAX(GREATEST(v1.y,v2.y)) - MIN(LEAST(v1.y,v2.y))))
             / (200.0 / v_world_height) AS fit
    FROM linedefs ld
    JOIN vertexes v1 ON v1.map_id = ld.map_id AND v1.id = ld.v1_id
    JOIN vertexes v2 ON v2.map_id = ld.map_id AND v2.id = ld.v2_id
    WHERE ld.map_id = a_map_id
  ) b
  WHERE av.map_id = a_map_id AND av.player_thing_id = a_player_thing_id;
  RETURN 1;
END;;

