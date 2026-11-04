-- What the 3D renderer's two per-tic queries share (play.gr runs each as this, then its own part): the frame's
-- context and clock, the camera, the sectors as lit this tic. Followed by sql/client/geometry.sql (walls, floors,
-- ceilings) or sql/client/things.sql (what stands in the world).
--
-- Reads @map_id, @player_thing_id, @skill (0..4), the camera @x @y @z (eye) and @angle (degrees, Doom's: 0 east,
-- 90 north) and @tic (the gameplay clock, for animated lights, flats and scrolling walls).
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
)
