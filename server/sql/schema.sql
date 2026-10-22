-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Ronny Krueger
-- AMIVCS server database schema
-- Fully separate from AmiCloud's server/sql/schema.sql -- own database,
-- own users, no shared tables or foreign keys across the two.
-- Run as: mysql -u root -p < schema.sql

CREATE DATABASE IF NOT EXISTS amivcs
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE amivcs;

CREATE TABLE IF NOT EXISTS users (
    id         INT UNSIGNED     AUTO_INCREMENT PRIMARY KEY,
    username   VARCHAR(64)      NOT NULL UNIQUE,
    pass_hash  VARCHAR(255)     NOT NULL,
    created_at DATETIME         DEFAULT CURRENT_TIMESTAMP,
    active     TINYINT(1)       DEFAULT 1,
    display_name  VARCHAR(128) NOT NULL DEFAULT '',   -- ab hier migrate_001
    email         VARCHAR(255) NOT NULL DEFAULT '',
    is_admin      TINYINT(1)   NOT NULL DEFAULT 0,
    last_login    DATETIME     NULL DEFAULT NULL,
    session_epoch INT UNSIGNED NOT NULL DEFAULT 0     -- ++ macht Web-Sessions ungueltig
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- v1 has no shared/team repositories (docs/adr/09_ADR_VCS_Design.md) --
-- a repo name is unique per owner, not server-wide.
CREATE TABLE IF NOT EXISTS repos (
    id         INT UNSIGNED     AUTO_INCREMENT PRIMARY KEY,
    owner_id   INT UNSIGNED     NOT NULL,
    name       VARCHAR(128)     NOT NULL,
    created_at DATETIME         DEFAULT CURRENT_TIMESTAMP,
    description    VARCHAR(255) NOT NULL DEFAULT '',  -- ab hier migrate_001
    is_public      TINYINT(1)   NOT NULL DEFAULT 0,   -- im Browser fuer jeden sichtbar
    default_branch VARCHAR(191) NOT NULL DEFAULT 'refs/heads/main',
    pushed_at      DATETIME     NULL DEFAULT NULL,

    UNIQUE KEY uq_owner_name (owner_id, name),
    FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Dumb, content-addressed object store: one row per (repo, oid). Bytes
-- live on disk under AMIVCS_STORAGE_PATH/<owner>/<repo>/objects/<fan-out>/,
-- not in the DB -- this table is metadata (type/size) plus the existence
-- check used by action=has for push negotiation.
CREATE TABLE IF NOT EXISTS objects (
    id         BIGINT UNSIGNED  AUTO_INCREMENT PRIMARY KEY,
    repo_id    INT UNSIGNED     NOT NULL,
    oid        CHAR(32)         NOT NULL,          -- MD5 hex of the raw payload
    type       TINYINT UNSIGNED NOT NULL,          -- 0=blob, 1=tree, 2=commit
    size_bytes BIGINT UNSIGNED  NOT NULL DEFAULT 0,
    created_at DATETIME         DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY uq_repo_oid (repo_id, oid),
    FOREIGN KEY (repo_id) REFERENCES repos(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Branches. Updated only via compare-and-swap (RefStore::updateRef) --
-- the server never merges (ADR 09), it just refuses a non-fast-forward
-- push and lets the client pull+merge+retry.
CREATE TABLE IF NOT EXISTS refs (
    id         BIGINT UNSIGNED  AUTO_INCREMENT PRIMARY KEY,
    repo_id    INT UNSIGNED     NOT NULL,
    name       VARCHAR(191)     NOT NULL,          -- e.g. "refs/heads/main"
    oid        CHAR(32)         NOT NULL,
    updated_at DATETIME         DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY uq_repo_name (repo_id, name),
    FOREIGN KEY (repo_id) REFERENCES repos(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- App-Passwoerter und Login-Bremse (migrate_001)
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

-- Ersten Admin anlegen (nicht per SQL):
--   sudo -u apache php bin/amivcs-admin.php user:add --admin <name>
