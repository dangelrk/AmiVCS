-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Ronny Krueger
-- Migration 002: Branch-Namen binaer vergleichen. Vorher waren
-- "refs/heads/Main" und "refs/heads/main" fuer die Datenbank derselbe
-- Branch; ein Push des einen konnte so den anderen ueberschreiben.
-- Einspielen mit: mysql -u root -p amivcs < migrate_002.sql

ALTER TABLE refs
    MODIFY name VARBINARY(191) NOT NULL;
