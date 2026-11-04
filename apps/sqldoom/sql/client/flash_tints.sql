-- Doom's screen flashes for the true-colour renderer: each of PLAYPAL's palettes 1..13 (8 red for pain, 4 gold for
-- pickups, 1 green for the radiation suit) as the colour it pulls the base palette toward and by how much, worked out
-- from the WAD's own palettes (colormap_rgb, level 0: the unshaded colours).
--
-- Doom made those palettes by blending every base colour toward one tint (new = base * (1 - amount) + tint * amount),
-- so a least-squares line per channel, palette p against palette 0 over the 256 entries, gives slope 1 - amount and
-- intercept tint * amount. The 3D picture is not palette indices, so play.gr lays the tint over it at that amount.
WITH pairs AS (
  SELECT p.pal, b.r * 1e0 AS xr, b.g * 1e0 AS xg, b.b * 1e0 AS xb, p.r * 1e0 AS yr, p.g * 1e0 AS yg, p.b * 1e0 AS yb
  FROM colormap_rgb p
  JOIN colormap_rgb b ON b.pal = 0 AND b.level = 0 AND b.palette_index = p.palette_index
  WHERE p.level = 0 AND p.pal > 0
), fit AS (
  SELECT pal,
    (COUNT(*) * SUM(xr * yr) - SUM(xr) * SUM(yr)) / NULLIF(COUNT(*) * SUM(xr * xr) - SUM(xr) * SUM(xr), 0) AS sr,
    (COUNT(*) * SUM(xg * yg) - SUM(xg) * SUM(yg)) / NULLIF(COUNT(*) * SUM(xg * xg) - SUM(xg) * SUM(xg), 0) AS sg,
    (COUNT(*) * SUM(xb * yb) - SUM(xb) * SUM(yb)) / NULLIF(COUNT(*) * SUM(xb * xb) - SUM(xb) * SUM(xb), 0) AS sb,
    AVG(xr) AS mxr, AVG(xg) AS mxg, AVG(xb) AS mxb, AVG(yr) AS myr, AVG(yg) AS myg, AVG(yb) AS myb
  FROM pairs
  GROUP BY pal
), amounts AS (
  SELECT f.*, 1e0 - (f.sr + f.sg + f.sb) / 3e0 AS amount FROM fit f
)
SELECT pal, amount,
  CAST(ROUND(LEAST(255e0, GREATEST(0e0, (myr - sr * mxr) / NULLIF(amount, 0)))) AS SIGNED) AS r,
  CAST(ROUND(LEAST(255e0, GREATEST(0e0, (myg - sg * mxg) / NULLIF(amount, 0)))) AS SIGNED) AS g,
  CAST(ROUND(LEAST(255e0, GREATEST(0e0, (myb - sb * mxb) / NULLIF(amount, 0)))) AS SIGNED) AS b
FROM amounts
WHERE amount > 0
ORDER BY pal
