-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Ronny Krueger
-- Migration 001: Weboberflaeche -- Benutzerverwaltung, App-Passwoerter,
-- Login-Bremse, oeffentliche Repositories (docs/design/webui-plan.md).
-- Einspielen mit: mysql -u root -p amivcs < migrate_001.sql
-- Idempotenz: bricht ab, wenn die Spalten schon existieren -- vorher pruefen mit
--   SHOW COLUMNS FROM users LIKE 'is_admin';
-- Danach den ersten Admin befoerdern bzw. anlegen:
--   sudo -u apache php bin/amivcs-admin.php user:admin <name> on

ALTER TABLE users
    ADD COLUMN display_name  VARCHAR(128) NOT NULL DEFAULT '',
    ADD COLUMN email         VARCHAR(255) NOT NULL DEFAULT '',
    ADD COLUMN is_admin      TINYINT(1)   NOT NULL DEFAULT 0,
    ADD COLUMN last_login    DATETIME     NULL DEFAULT NULL,
    ADD COLUMN session_epoch INT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'Erhoeht bei Passwortwechsel/Deaktivierung -> Web-Sessions ungueltig';

ALTER TABLE repos
    ADD COLUMN description    VARCHAR(255) NOT NULL DEFAULT '',
    ADD COLUMN is_public      TINYINT(1)   NOT NULL DEFAULT 0
        COMMENT '1 = im Browser fuer jeden sichtbar; API bleibt Eigentuemer-only',
    ADD COLUMN default_branch VARCHAR(191) NOT NULL DEFAULT 'refs/heads/main',
    ADD COLUMN pushed_at      DATETIME     NULL DEFAULT NULL;

CREATE TABLE IF NOT EXISTS app_passwords (
    id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id      INT UNSIGNED NOT NULL,
    name         VARCHAR(64)  NOT NULL,
    token_hash   CHAR(64)     NOT NULL UNIQUE,  -- SHA-256 des Tokens
    created_at   DATETIME     DEFAULT CURRENT_TIMESTAMP,
    last_used_at DATETIME     NULL DEFAULT NULL,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS auth_failures (
    id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    ip       VARCHAR(45)  NOT NULL,
    scope    VARCHAR(96)  NOT NULL,  -- Benutzername
    at       DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_ip_at (ip, at),
    KEY idx_ip_scope_at (ip, scope, at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
