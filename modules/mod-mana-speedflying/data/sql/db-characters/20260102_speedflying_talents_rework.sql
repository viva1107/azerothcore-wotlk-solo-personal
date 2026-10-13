-- Speedflying talent tree, rework: 12 lifetime points on a rising yard curve, four rows per branch
-- with a pick of two on row 2 and a single-rank capstone on row 4.
--
-- The old tree was a flat 250 yards per point over 15 talents in three tiers, and its rank ids and
-- tiers are not the new ones (a stored rank would land on a different talent), so the progression is
-- deliberately reset. This runs after 20260101_speedflying_talents.sql, which only creates the tables.
--
-- Idempotent in the sense that a re-apply is harmless to an empty table, but it is a one-shot
-- migration: delete this file once every realm has run it, or a later re-apply wipes the new tree too.
DELETE FROM `character_speedflying_talents`;
DELETE FROM `character_speedflying`;