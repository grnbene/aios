-- Pick Rush: one game tic (1/30 s), as ASQL stored functions. A statement ends at `;;`.
--
--   SELECT pr_tic(throttle, steer, lift, action)
--     throttle -1..1 (back, forward), steer -1..1 (left, right), lift -1..1 (lower, raise), action 1 to pick up or
--     set down this tic. Returns the tic number.
--
-- The player's forklift is entity 1; the other forklifts are bots, driving the aisle network. A pallet rides the
-- forks once picked; set down in an empty slot it is put away, on the shipping dock it fills an order line.

-- Whether a forklift body (a circle of radius 0.85 round px, pz, 2.2 m tall) standing there would hit anything:
-- a solid static box, another forklift, or a pallet standing on the floor.
DROP FUNCTION IF EXISTS pr_blocked;;
CREATE FUNCTION pr_blocked(px DOUBLE, pz DOUBLE, me INT) RETURNS INT
BEGIN
  DECLARE v_n INT DEFAULT 0;
  SELECT COUNT(*) INTO v_n FROM solids s
  WHERE s.minx < px + 0.85 AND s.maxx > px - 0.85 AND s.minz < pz + 0.85 AND s.maxz > pz - 0.85
    AND s.miny < 2.2 AND s.maxy > 0.05
    AND POW(GREATEST(s.minx - px, 0, px - s.maxx), 2) + POW(GREATEST(s.minz - pz, 0, pz - s.maxz), 2) < 0.7225;
  IF v_n > 0 THEN RETURN 1; END IF;
  SELECT COUNT(*) INTO v_n FROM entities e
  WHERE e.kind IN ('player', 'bot') AND e.entity_id <> me
    AND POW(e.x - px, 2) + POW(e.z - pz, 2) < 2.89;
  IF v_n > 0 THEN RETURN 1; END IF;
  SELECT COUNT(*) INTO v_n FROM entities e
  WHERE e.kind = 'pallet' AND e.slot_id IS NULL AND e.carried_by IS NULL
    AND POW(e.x - px, 2) + POW(e.z - pz, 2) < 1.96;
  RETURN IF(v_n > 0, 1, 0);
END;;

DROP FUNCTION IF EXISTS pr_event;;
CREATE FUNCTION pr_event(a_tic INT, a_msg VARCHAR(120)) RETURNS INT
BEGIN
  INSERT INTO events (tic, msg) VALUES (a_tic, a_msg);
  RETURN 1;
END;;

-- The player picks up or sets down at the forks, which are at (fx, fz), fork_h up.
DROP FUNCTION IF EXISTS pr_action;;
CREATE FUNCTION pr_action(a_tic INT, fx DOUBLE, fz DOUBLE, fh DOUBLE, a_yaw DOUBLE) RETURNS INT
BEGIN
  DECLARE v_carry INT DEFAULT NULL;
  DECLARE v_pid INT DEFAULT NULL;
  DECLARE v_sid INT DEFAULT NULL;
  DECLARE v_sku VARCHAR(16) DEFAULT NULL;
  DECLARE v_zone VARCHAR(16) DEFAULT NULL;
  DECLARE v_label VARCHAR(16) DEFAULT NULL;
  DECLARE v_oid INT DEFAULT NULL;
  DECLARE v_line INT DEFAULT NULL;
  DECLARE v_left INT DEFAULT 0;
  DECLARE v_due INT DEFAULT 0;
  DECLARE v_bonus INT DEFAULT 0;
  DECLARE v_near INT DEFAULT 0;
  DECLARE v_squared INT DEFAULT 0;

  -- square to a rack face: facing north or south (a single-deep rack is open to both aisles) within about 20
  -- degrees
  SET v_squared = IF(ABS(COS(a_yaw)) > 0.94, 1, 0);
  SELECT entity_id, sku, zone INTO v_carry, v_sku, v_zone FROM entities WHERE carried_by = 1 LIMIT 1;

  IF v_carry IS NULL THEN
    SELECT e.entity_id, e.slot_id, e.sku INTO v_pid, v_sid, v_sku FROM entities e
    WHERE e.kind = 'pallet' AND e.carried_by IS NULL
      AND POW(e.x - fx, 2) + POW(e.z - fz, 2) < 0.36 AND ABS(e.y - fh) < 0.35
      AND (e.slot_id IS NULL OR v_squared = 1)
    ORDER BY POW(e.x - fx, 2) + POW(e.z - fz, 2) LIMIT 1;
    IF v_pid IS NULL THEN
      SELECT COUNT(*) INTO v_near FROM entities e
      WHERE e.kind = 'pallet' AND e.carried_by IS NULL AND POW(e.x - fx, 2) + POW(e.z - fz, 2) < 0.36;
      RETURN pr_event(a_tic, IF(v_near > 0, IF(v_squared = 0, 'Square up to the rack first.', 'Forks at the wrong height for that pallet.'),
                                 'No pallet on the forks there.'));
    END IF;
    SELECT label INTO v_label FROM slots WHERE slot_id = v_sid;
    UPDATE entities SET carried_by = 1, slot_id = NULL WHERE entity_id = v_pid;
    UPDATE slots SET pallet_id = NULL WHERE slot_id = v_sid;
    RETURN pr_event(a_tic, CONCAT('Picked ', v_sku, IF(v_label IS NULL, ' off the floor', CONCAT(' from ', v_label)), '.'));
  END IF;

  -- On the shipping dock, with the forks down: it fills the oldest open order line that wants it.
  IF fx BETWEEN 34 AND 42 AND fz BETWEEN 26 AND 34 THEN
    IF fh > 0.5 THEN RETURN pr_event(a_tic, 'Lower the forks to ship it.'); END IF;
    SELECT l.order_id, l.line_no INTO v_oid, v_line FROM order_lines l JOIN orders o ON o.order_id = l.order_id
    WHERE o.status = 'open' AND l.sku = v_sku AND l.picked < l.need
    ORDER BY o.due_tic, l.order_id, l.line_no LIMIT 1;
    IF v_oid IS NULL THEN RETURN pr_event(a_tic, CONCAT('No open order wants ', v_sku, '.')); END IF;
    UPDATE order_lines SET picked = picked + 1 WHERE order_id = v_oid AND line_no = v_line;
    DELETE FROM parts WHERE entity_id = v_carry;
    DELETE FROM entities WHERE entity_id = v_carry;
    UPDATE game SET score = score + 25 WHERE id = 1;
    SELECT COUNT(*) INTO v_left FROM order_lines WHERE order_id = v_oid AND picked < need;
    IF v_left = 0 THEN
      SELECT due_tic INTO v_due FROM orders WHERE order_id = v_oid;
      SET v_bonus = 100 + GREATEST(0, (v_due - a_tic) DIV 30);
      UPDATE orders SET status = 'shipped', closed_tic = a_tic WHERE order_id = v_oid;
      UPDATE game SET score = score + v_bonus, shipped = shipped + 1 WHERE id = 1;
      RETURN pr_event(a_tic, CONCAT('Order ', v_oid, ' shipped! +', v_bonus + 25));
    END IF;
    RETURN pr_event(a_tic, CONCAT('Loaded ', v_sku, ' for order ', v_oid, ', ', v_left, ' line(s) to go. +25'));
  END IF;

  -- Into an empty slot the forks are in front of, at its height.
  SELECT s.slot_id, s.label INTO v_sid, v_label FROM slots s
  WHERE s.pallet_id IS NULL AND POW(s.x - fx, 2) + POW(s.z - fz, 2) < 0.36 AND ABS(s.y - fh) < 0.35
  ORDER BY POW(s.x - fx, 2) + POW(s.z - fz, 2) LIMIT 1;
  IF v_sid IS NOT NULL THEN
    IF v_squared = 0 THEN RETURN pr_event(a_tic, 'Square up to the rack first.'); END IF;
    UPDATE entities e JOIN slots s ON s.slot_id = v_sid
      SET e.x = s.x, e.y = s.y, e.z = s.z, e.yaw = 0, e.carried_by = NULL, e.slot_id = s.slot_id, e.zone = NULL
    WHERE e.entity_id = v_carry;
    UPDATE slots SET pallet_id = v_carry WHERE slot_id = v_sid;
    IF v_zone = 'inbound' THEN
      UPDATE game SET score = score + 10, putaways = putaways + 1 WHERE id = 1;
      RETURN pr_event(a_tic, CONCAT('Put away ', v_sku, ' in ', v_label, '. +10'));
    END IF;
    RETURN pr_event(a_tic, CONCAT('Set ', v_sku, ' in ', v_label, '.'));
  END IF;

  -- Anywhere else on the floor, forks down and clear of the racks.
  SELECT COUNT(*) INTO v_near FROM slots s WHERE POW(s.x - fx, 2) + POW(s.z - fz, 2) < 2.0;
  IF v_near > 0 THEN
    RETURN pr_event(a_tic, IF(fh < 0.35, 'That slot is taken.', 'No empty slot at that height.'));
  END IF;
  IF fh > 0.3 THEN RETURN pr_event(a_tic, 'Lower the forks to set it on the floor.'); END IF;
  UPDATE entities SET carried_by = NULL, y = 0 WHERE entity_id = v_carry;
  RETURN pr_event(a_tic, CONCAT('Set ', v_sku, ' on the floor.'));
END;;

-- The other forklifts: each drives to its next waypoint, waits for whoever is in front of it, and at a waypoint
-- turns onto a neighbouring aisle (back the way it came only at a dead end).
DROP TABLE IF EXISTS bot_step;;
CREATE TABLE bot_step (
  entity_id INT PRIMARY KEY,
  nx DOUBLE NOT NULL, nz DOUBLE NOT NULL, nyaw DOUBLE NOT NULL,
  arrived INT NOT NULL,
  next_wp INT NULL
);;

DROP FUNCTION IF EXISTS pr_bots;;
CREATE FUNCTION pr_bots(a_tic INT) RETURNS INT
BEGIN
  DELETE FROM bot_step;
  INSERT INTO bot_step (entity_id, nx, nz, nyaw, arrived)
  SELECT m.entity_id,
         m.x + m.ux * LEAST(m.dist, m.step), m.z + m.uz * LEAST(m.dist, m.step),
         ATAN2(m.ux, m.uz),
         IF(m.dist <= m.step, 1, 0)
  FROM (
    SELECT e.entity_id, e.x, e.z,
           SQRT(POW(w.x - e.x, 2) + POW(w.z - e.z, 2)) AS dist,
           (w.x - e.x) / GREATEST(SQRT(POW(w.x - e.x, 2) + POW(w.z - e.z, 2)), 0.0001) AS ux,
           (w.z - e.z) / GREATEST(SQRT(POW(w.x - e.x, 2) + POW(w.z - e.z, 2)), 0.0001) AS uz,
           2.4 / 30 AS step
    FROM entities e JOIN bots b ON b.entity_id = e.entity_id JOIN waypoints w ON w.wp_id = b.target_wp
    WHERE b.wait_tics = 0
  ) m
  -- whoever is close ahead of it (the player, or a bot with a lower id) holds it up
  WHERE NOT EXISTS (
    SELECT 1 FROM entities o
    WHERE o.entity_id <> m.entity_id AND (o.kind = 'player' OR (o.kind = 'bot' AND o.entity_id < m.entity_id))
      AND POW(o.x - m.x, 2) + POW(o.z - m.z, 2) < 10.24
      AND (o.x - m.x) * m.ux + (o.z - m.z) * m.uz > 0.5);

  UPDATE bot_step st JOIN bots b ON b.entity_id = st.entity_id
  SET st.next_wp = (
    SELECT ed.b FROM wp_edges ed WHERE ed.a = b.target_wp
    ORDER BY IF(ed.b = b.prev_wp, 1, 0), pr_rand(ed.b + 100 * b.entity_id, a_tic) LIMIT 1)
  WHERE st.arrived = 1;

  UPDATE entities e JOIN bot_step st ON st.entity_id = e.entity_id
  SET e.x = st.nx, e.z = st.nz, e.yaw = st.nyaw, e.speed = 2.4;
  UPDATE bots b JOIN bot_step st ON st.entity_id = b.entity_id
  SET b.prev_wp = b.target_wp, b.target_wp = st.next_wp, b.wait_tics = 20
  WHERE st.arrived = 1;
  UPDATE bots SET wait_tics = wait_tics - 1 WHERE wait_tics > 0;
  -- Held up for a second and a half, it gives way: back the way it came (MySQL's assignments run left to right,
  -- so this swaps the two waypoints).
  UPDATE bots SET held_tics = 0 WHERE entity_id IN (SELECT entity_id FROM bot_step);
  UPDATE bots SET held_tics = held_tics + 1 WHERE wait_tics = 0 AND entity_id NOT IN (SELECT entity_id FROM bot_step);
  UPDATE bots SET target_wp = target_wp + prev_wp, prev_wp = target_wp - prev_wp, target_wp = target_wp - prev_wp,
                  held_tics = 0
  WHERE held_tics > 45;
  RETURN (SELECT COUNT(*) FROM bot_step);
END;;

-- Orders come in when fewer than three are open (at most one each five seconds); an order that runs past its due
-- tic is late. Inbound pallets turn up at receiving every twenty seconds while fewer than three are waiting there.
DROP FUNCTION IF EXISTS pr_business;;
CREATE FUNCTION pr_business(a_tic INT) RETURNS INT
BEGIN
  DECLARE v_open INT DEFAULT 0;
  DECLARE v_lines INT DEFAULT 1;
  DECLARE v_oid INT DEFAULT NULL;
  DECLARE v_late INT DEFAULT 0;
  DECLARE v_waiting INT DEFAULT 0;
  DECLARE v_id INT DEFAULT 0;
  DECLARE v_spot INT DEFAULT NULL;

  UPDATE orders SET status = 'late', closed_tic = a_tic WHERE status = 'open' AND due_tic < a_tic;
  SET v_late = ROW_COUNT();
  IF v_late > 0 THEN
    UPDATE game SET score = score - 50 * v_late, late = late + v_late WHERE id = 1;
    DO pr_event(a_tic, CONCAT(v_late, ' order(s) went out late. -', 50 * v_late));
  END IF;

  SELECT COUNT(*) INTO v_open FROM orders WHERE status = 'open';
  IF v_open < 3 AND (v_open = 0 OR a_tic % 150 = 0) THEN
    SET v_lines = 1 + FLOOR(pr_rand(a_tic, 31) * 3);
    INSERT INTO orders (created_tic, due_tic) VALUES (a_tic, a_tic + 30 * (60 + 35 * v_lines));
    SET v_oid = LAST_INSERT_ID();
    -- lines for SKUs there is stock of, none twice, and none an open order has already claimed every pallet of
    INSERT INTO order_lines (order_id, line_no, sku, need)
    SELECT v_oid, ROW_NUMBER() OVER (ORDER BY st.r), st.sku, 1
    FROM (
      SELECT s.sku, pr_rand(CRC32(s.sku), a_tic) AS r
      FROM (SELECT sku, COUNT(*) AS n FROM entities WHERE kind = 'pallet' AND slot_id IS NOT NULL GROUP BY sku) s
      LEFT JOIN (SELECT l.sku, SUM(l.need - l.picked) AS claimed FROM order_lines l JOIN orders o ON o.order_id = l.order_id
                 WHERE o.status = 'open' GROUP BY l.sku) c ON c.sku = s.sku
      WHERE s.n > COALESCE(c.claimed, 0)
      ORDER BY r LIMIT v_lines
    ) st;
    IF ROW_COUNT() = 0 THEN
      DELETE FROM orders WHERE order_id = v_oid;
    ELSE
      DO pr_event(a_tic, CONCAT('New order ', v_oid, ': ',
        (SELECT GROUP_CONCAT(sku ORDER BY line_no SEPARATOR ', ') FROM order_lines WHERE order_id = v_oid), '.'));
    END IF;
  END IF;

  IF a_tic % 600 = 0 THEN
    SELECT COUNT(*) INTO v_waiting FROM entities WHERE zone = 'inbound' AND carried_by IS NULL;
    IF v_waiting < 3 THEN
      -- the first of the four receiving spots with nothing standing on it
      SELECT sp.n INTO v_spot FROM (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) sp
      WHERE NOT EXISTS (SELECT 1 FROM entities e WHERE e.kind IN ('pallet', 'player', 'bot')
                        AND POW(e.x - (35.5 + 2.4 * sp.n), 2) + POW(e.z - 4, 2) < 2.0)
      ORDER BY sp.n LIMIT 1;
      IF v_spot IS NOT NULL THEN
        SELECT next_entity INTO v_id FROM game WHERE id = 1;
        UPDATE game SET next_entity = next_entity + 1 WHERE id = 1;
        INSERT INTO entities (entity_id, kind, x, y, z, yaw, sku, zone)
        SELECT v_id, 'pallet', 35.5 + 2.4 * v_spot, 0, 4, 0, k.sku, 'inbound'
        FROM skus k ORDER BY pr_rand(CRC32(k.sku), a_tic) LIMIT 1;
        DO pr_add_pallet_parts(v_id);
        DO pr_event(a_tic, CONCAT('Inbound: ', (SELECT sku FROM entities WHERE entity_id = v_id), ' at receiving.'));
      END IF;
    END IF;
  END IF;

  IF a_tic % 300 = 0 THEN
    DELETE FROM events WHERE event_id < (SELECT m FROM (SELECT MAX(event_id) - 40 AS m FROM events) x);
  END IF;
  RETURN 1;
END;;

DROP FUNCTION IF EXISTS pr_tic;;
CREATE FUNCTION pr_tic(a_thr DOUBLE, a_steer DOUBLE, a_lift DOUBLE, a_act INT) RETURNS INT
BEGIN
  DECLARE v_tic INT DEFAULT 0;
  DECLARE v_x DOUBLE DEFAULT 0;
  DECLARE v_z DOUBLE DEFAULT 0;
  DECLARE v_yaw DOUBLE DEFAULT 0;
  DECLARE v_speed DOUBLE DEFAULT 0;
  DECLARE v_fh DOUBLE DEFAULT 0;
  DECLARE v_nx DOUBLE DEFAULT 0;
  DECLARE v_nz DOUBLE DEFAULT 0;
  DECLARE v_dt DOUBLE DEFAULT 0.0333333333333333;

  UPDATE game SET tic = tic + 1 WHERE id = 1;
  SELECT tic INTO v_tic FROM game WHERE id = 1;
  SELECT x, z, yaw, speed, fork_h INTO v_x, v_z, v_yaw, v_speed, v_fh FROM entities WHERE entity_id = 1;

  -- Drive: up to 5 m/s forward, 2.5 back; it coasts to a stop with no throttle. It steers in place slowly and
  -- faster on the move, the other way round in reverse.
  SET v_speed = v_speed + (a_thr * 8 - v_speed * IF(a_thr = 0, 4, 1.6)) * v_dt;
  SET v_speed = LEAST(5, GREATEST(-2.5, v_speed));
  IF ABS(v_speed) < 0.02 AND a_thr = 0 THEN SET v_speed = 0; END IF;
  SET v_yaw = v_yaw + a_steer * 1.7 * v_dt * (0.35 + 0.65 * LEAST(1, ABS(v_speed) / 2)) * IF(v_speed < -0.1, -1, 1);
  SET v_nx = v_x + SIN(v_yaw) * v_speed * v_dt;
  SET v_nz = v_z + COS(v_yaw) * v_speed * v_dt;
  IF v_speed <> 0 AND pr_blocked(v_nx, v_nz, 1) = 1 THEN
    IF pr_blocked(v_nx, v_z, 1) = 0 THEN SET v_nz = v_z;
    ELSEIF pr_blocked(v_x, v_nz, 1) = 0 THEN SET v_nx = v_x;
    ELSE SET v_nx = v_x; SET v_nz = v_z;
    END IF;
    -- sliding along a wall keeps what speed goes along it; driving straight into one stops it
    IF ABS(v_nx - v_x) + ABS(v_nz - v_z) < 0.001 THEN SET v_speed = 0; END IF;
  END IF;
  SET v_fh = LEAST(4.2, GREATEST(0, v_fh + a_lift * 1.1 * v_dt));
  UPDATE entities SET x = v_nx, z = v_nz, yaw = v_yaw, speed = v_speed, fork_h = v_fh WHERE entity_id = 1;
  -- what it carries goes with the forks
  UPDATE entities SET x = v_nx + SIN(v_yaw) * 1.45, z = v_nz + COS(v_yaw) * 1.45, y = v_fh + 0.075, yaw = v_yaw
  WHERE carried_by = 1;

  IF a_act = 1 THEN
    DO pr_action(v_tic, v_nx + SIN(v_yaw) * 1.45, v_nz + COS(v_yaw) * 1.45, v_fh, v_yaw);
  END IF;
  DO pr_bots(v_tic);
  DO pr_business(v_tic);
  RETURN v_tic;
END;;
