-- SQLDoom's light tables (sql/runtime/functions/43_render_light.sql) as ASQL stored functions. A statement ends at
-- `;;` in this file (the loader splits there).

DROP FUNCTION IF EXISTS doom_light_index;;
-- R_InitLightTables: a COLORMAP row from the sector's light, the extra light (weapon flash, visor) and the
-- attenuation for the distance, 0 brightest .. 31 darkest.
CREATE FUNCTION doom_light_index(p_sector_light DOUBLE, p_extra INT, p_attenuation INT) RETURNS INT DETERMINISTIC
RETURN GREATEST(0, LEAST(31,
  (15 - LEAST(15, GREATEST(0, FLOOR(p_sector_light / 16e0) + p_extra))) * 4
  - p_attenuation));;

DROP FUNCTION IF EXISTS doom_light_scale;;
-- scalelight, for walls and sprites: attenuation by 1/depth. A depth of 0 attenuates nothing (PostgreSQL's GREATEST
-- passes over the NULL the division gives; MySQL's would return it).
CREATE FUNCTION doom_light_scale(p_depth DOUBLE) RETURNS INT DETERMINISTIC
RETURN FLOOR(LEAST(47e0, GREATEST(0e0, COALESCE(2560e0 / NULLIF(p_depth, 0e0), 0e0))) / 2e0);;

DROP FUNCTION IF EXISTS doom_light_zdepth;;
-- zlight, for planes, which vanilla indexes by a distance step instead.
CREATE FUNCTION doom_light_zdepth(p_depth DOUBLE) RETURNS INT DETERMINISTIC
RETURN FLOOR(80e0 / (LEAST(127, FLOOR(p_depth / 16e0)) + 1));;
