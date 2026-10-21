-- Route Builder v2 close-out fixture for local ASQL.
-- Mirrors v8/server/sql/route_builder_v2_schema.sql
-- (v8 is a SIBLING tree of this workdir, not a subdirectory of it) for exactly the two tables
-- RB2DispatchStatusRollup::decide() reads, and nothing else.
--
-- Enum + column names copied verbatim from the production schema so the rule is
-- exercised against the real shape, not a paraphrase of it.

DROP DATABASE IF EXISTS rb2;
CREATE DATABASE rb2;
USE rb2;

CREATE TABLE dispatch (
    dispatch_id  INT UNSIGNED NOT NULL AUTO_INCREMENT,
    plan_id      INT UNSIGNED NULL,
    status_id    INT UNSIGNED NOT NULL DEFAULT 1,
    invoiced     TINYINT(1) NOT NULL DEFAULT 0,
    dateupdated  DATETIME NULL,
    PRIMARY KEY (dispatch_id)
);

CREATE TABLE dispatch_stop (
    stop_id           INT UNSIGNED NOT NULL AUTO_INCREMENT,
    dispatch_id       INT UNSIGNED NOT NULL,
    plan_id           INT UNSIGNED NULL,
    execution_status  ENUM('pending','en_route','arrived','completed','skipped','failed')
                      NOT NULL DEFAULT 'pending',
    execution_outcome VARCHAR(48) NULL,
    executed_at       DATETIME NULL,
    executed_by       VARCHAR(64) NULL,
    dateupdated       DATETIME NULL,
    PRIMARY KEY (stop_id)
);

-- Seven dispatches, one per branch of decide(). Expected verdicts are asserted
-- by tools/rb2-closeout/verify.sh, not by this file.

-- D1: ten stops, all completed. The lazy-driver happy path -> CLOSE.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (1, 100, 5, 0, NOW());
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, executed_at, executed_by, dateupdated) VALUES
 (1,100,'completed','2026-09-29 09:05','dave',NOW()),
 (1,100,'completed','2026-09-29 09:20','dave',NOW()),
 (1,100,'completed','2026-09-29 09:40','dave',NOW()),
 (1,100,'completed','2026-09-29 10:00','dave',NOW()),
 (1,100,'completed','2026-09-29 10:15','dave',NOW()),
 (1,100,'completed','2026-09-29 10:35','dave',NOW()),
 (1,100,'completed','2026-09-29 10:50','dave',NOW()),
 (1,100,'completed','2026-09-29 11:05','dave',NOW()),
 (1,100,'completed','2026-09-29 11:25','dave',NOW()),
 (1,100,'completed','2026-09-29 11:40','dave',NOW());

-- D2: driver marked 8 of 10 and went home. 2 still open -> HOLD. Mode A.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (2, 101, 5, 0, '2026-09-29 08:00');
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, executed_at, executed_by, dateupdated) VALUES
 (2,101,'completed','2026-09-29 09:10','dave',NOW()),
 (2,101,'completed','2026-09-29 09:30','dave',NOW()),
 (2,101,'completed','2026-09-29 09:55','dave',NOW()),
 (2,101,'completed','2026-09-29 10:10','dave',NOW()),
 (2,101,'completed','2026-09-29 10:30','dave',NOW()),
 (2,101,'completed','2026-09-29 10:50','dave',NOW()),
 (2,101,'completed','2026-09-29 11:10','dave',NOW()),
 (2,101,'completed','2026-09-29 11:30','dave',NOW()),
 (2,101,'en_route',NULL,NULL,'2026-09-29 11:35'),
 (2,101,'pending',NULL,NULL,'2026-09-29 08:00');

-- D3: one stop never touched at all. The stuck-stop case. Mode B. -> HOLD.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (3, 102, 5, 0, '2026-09-29 07:00');
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, executed_at, executed_by, dateupdated) VALUES
 (3,102,'completed','2026-09-29 09:00','dave',NOW()),
 (3,102,'completed','2026-09-29 09:20','dave',NOW()),
 (3,102,'completed','2026-09-29 09:45','dave',NOW()),
 (3,102,'completed','2026-09-29 10:05','dave',NOW()),
 (3,102,'failed','2026-09-29 10:25','dave',NOW()),
 (3,102,'skipped','2026-09-29 10:35','dave',NOW()),
 (3,102,'pending',NULL,NULL,'2026-09-29 07:00');

-- D4: no contact, refusal, failed, skipped, rest completed. None open -> CLOSE.
-- This is the branch that matters: 'failed' and 'skipped' are FINAL, not open.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (4, 103, 5, 0, '2026-09-29 06:00');
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, execution_outcome, executed_at, executed_by, dateupdated) VALUES
 (4,103,'completed','signed_for','2026-09-29 09:00','dave',NOW()),
 (4,103,'failed','refused','2026-09-29 09:20','dave',NOW()),
 (4,103,'skipped','closed','2026-09-29 09:25','dave',NOW()),
 (4,103,'completed','met','2026-09-29 09:50','dave',NOW()),
 (4,103,'failed','no_contact','2026-09-29 10:10','dave',NOW()),
 (4,103,'completed','signed_for','2026-09-29 10:40','dave',NOW());

-- D5: already at the delivered status 31 -> already_delivered, no double bump.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (5, 104, 31, 0, '2026-09-28 18:00');
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, executed_at, executed_by, dateupdated) VALUES
 (5,104,'completed','2026-09-28 10:00','dave',NOW()),
 (5,104,'completed','2026-09-28 10:30','dave',NOW());

-- D6: already invoiced. The rollup must never touch this one.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (6, 105, 5, 1, '2026-09-28 19:00');
INSERT INTO dispatch_stop (dispatch_id, plan_id, execution_status, executed_at, executed_by, dateupdated) VALUES
 (6,105,'completed','2026-09-28 11:00','dave',NOW()),
 (6,105,'completed','2026-09-28 11:30','dave',NOW());

-- D7: dispatch exists, zero stops. -> no_stops, never close an empty run.
INSERT INTO dispatch (dispatch_id, plan_id, status_id, invoiced, dateupdated)
VALUES (7, 106, 5, 0, '2026-09-29 05:00');
