-- Speedflying talent tree: per-character points and ranks.
--
-- Idempotent and re-runnable: the module SQL updater may apply a module file twice, and a second run
-- must be a no-op. Nothing here is ever dropped, so a re-apply never costs a player their tree.
--
-- `points` is lifetime points earned; unspent points are derived (points - SUM(rank)) and are never
-- stored, so the two cannot drift. `yard_partial` carries the fraction under a whole point across a
-- flush and across a relog.
--
-- `character_speedflying_talents` holds a row only while rank >= 1; a respec deletes them. talent_id
-- is the module's own id (1..15), not a DBC id.

CREATE TABLE IF NOT EXISTS `character_speedflying` (
  `guid`         INT UNSIGNED NOT NULL,
  `points`       INT UNSIGNED NOT NULL DEFAULT 0,
  `yard_partial` DOUBLE NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `character_speedflying_talents` (
  `guid`      INT UNSIGNED NOT NULL,
  `talent_id` SMALLINT UNSIGNED NOT NULL,
  `rank`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`,`talent_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;