<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* GraphLayout.php -- PHP port of libcloudvcsgui/src/graph.c's lane-
 * packing algorithm, combined with the frontier/date-order commit walk
 * from libcloudvcs/src/repo.c's vcs_repo_log_all_raw() -- the WebUI has
 * no separate "raw log" vs. "graph layout" split the C API needs (no
 * CLI to keep narrow/console-only here), so both are one pass. Same
 * narrow, deliberate ADR 09 exception as ObjectDecoder.php: read-only
 * display, no VCS decision-making. Keep in sync with both C originals
 * (repo.c's vcs_repo_log_all_raw(), graph.c's vcsgui_compute_graph_layout())
 * if either ever changes. */
class GraphLayout
{
    /* refs: list of ['name'=>string,'oid'=>32-hex-char string ('' for an
     * empty branch)]. readCommit: string oid -> decoded commit array
     * (ObjectDecoder::decodeCommit()'s shape) or null if unreadable.
     *
     * Returns ['rows'=>list of decorated+laned commit rows (newest/
     * highest-priority date first), 'max_lanes'=>int]. Each row:
     *   oid, tree, parents, mtime, author, message, decoration,
     *   lane, parent_lanes, active_lanes.
     * $limit > 0 stops the walk after that many commits (the WebUI pages
     * through big histories); lanes stay correct for the rows returned. */
    public static function build(array $refs, callable $readCommit, int $limit = 0): array
    {
        $queued  = [];
        $visited = [];
        $frontier = []; /* list of ['oid'=>, 'mtime'=>] */

        foreach ($refs as $r) {
            $oid = $r['oid'];
            if ($oid === '' || isset($queued[$oid])) continue;
            $c = $readCommit($oid);
            if ($c === null) continue;
            $frontier[] = ['oid' => $oid, 'mtime' => $c['mtime']];
            $queued[$oid] = true;
        }

        $commits = [];
        $order   = [];

        while ($frontier !== []) {
            $bestIdx = 0;
            for ($i = 1; $i < count($frontier); $i++) {
                if ($frontier[$i]['mtime'] > $frontier[$bestIdx]['mtime']
                    || ($frontier[$i]['mtime'] === $frontier[$bestIdx]['mtime']
                        && $frontier[$i]['oid'] > $frontier[$bestIdx]['oid'])) {
                    $bestIdx = $i;
                }
            }
            $cur = $frontier[$bestIdx]['oid'];
            array_splice($frontier, $bestIdx, 1);

            if (isset($visited[$cur])) continue;
            $visited[$cur] = true;

            $c = $readCommit($cur);
            if ($c === null) continue;
            $commits[$cur] = $c;
            $order[] = $cur;
            if ($limit > 0 && count($order) >= $limit) {
                break;
            }

            foreach ($c['parents'] as $p) {
                if (isset($queued[$p])) continue;
                $pc = $readCommit($p);
                if ($pc === null) continue;
                $frontier[] = ['oid' => $p, 'mtime' => $pc['mtime']];
                $queued[$p] = true;
            }
        }

        $decorations = array_fill_keys($order, []);
        foreach ($refs as $r) {
            if ($r['oid'] !== '' && isset($decorations[$r['oid']])) {
                $short = $r['name'];
                if (strncmp($short, 'refs/heads/', 11) === 0) $short = substr($short, 11);
                $decorations[$r['oid']][] = $short;
            }
        }

        $laneWaiting = []; /* laneIndex => oid, '' = free */
        $maxLanes    = 0;
        $rows        = [];

        foreach ($order as $oid) {
            $c = $commits[$oid];

            $primary = null;
            foreach ($laneWaiting as $idx => $w) {
                if ($w === $oid) { $primary = $idx; break; }
            }
            if ($primary === null) {
                foreach ($laneWaiting as $idx => $w) {
                    if ($w === '') { $primary = $idx; break; }
                }
                if ($primary === null) {
                    $primary = count($laneWaiting);
                    $laneWaiting[$primary] = '';
                }
            }

            foreach ($laneWaiting as $idx => $w) {
                if ($idx !== $primary && $w === $oid) $laneWaiting[$idx] = '';
            }

            $activeLanes = [];
            foreach ($laneWaiting as $idx => $w) {
                if ($idx === $primary || $w !== '') $activeLanes[] = $idx;
            }

            $parentLanes = [];
            if (count($c['parents']) === 0) {
                $laneWaiting[$primary] = '';
            } else {
                $laneWaiting[$primary] = $c['parents'][0];
                $parentLanes[] = $primary;

                for ($k = 1; $k < count($c['parents']); $k++) {
                    $p = $c['parents'][$k];
                    $plane = null;
                    foreach ($laneWaiting as $idx => $w) {
                        if ($w === $p) { $plane = $idx; break; }
                    }
                    if ($plane === null) {
                        foreach ($laneWaiting as $idx => $w) {
                            if ($w === '') { $plane = $idx; break; }
                        }
                        if ($plane === null) $plane = count($laneWaiting);
                        $laneWaiting[$plane] = $p;
                    }
                    $parentLanes[] = $plane;
                }
            }

            if (count($laneWaiting) > $maxLanes) $maxLanes = count($laneWaiting);

            $rows[] = [
                'oid'          => $oid,
                'tree'         => $c['tree'],
                'parents'      => $c['parents'],
                'mtime'        => $c['mtime'],
                'author'       => $c['author'],
                'message'      => $c['message'],
                'decoration'   => implode(', ', $decorations[$oid]),
                'lane'         => $primary,
                'parent_lanes' => $parentLanes,
                'active_lanes' => $activeLanes,
            ];
        }

        return ['rows' => $rows, 'max_lanes' => $maxLanes];
    }
}
