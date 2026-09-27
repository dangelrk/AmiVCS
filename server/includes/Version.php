<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Central app version -- change here, NOT in config.php (a per-deployment
 * settings file). Guard mirrors AmiCloud's Version.php. */
if (!defined('AMIVCS_VERSION')) {
    define('AMIVCS_VERSION', '0.1');
}
