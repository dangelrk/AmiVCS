<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Zeilenweiser Diff fuer die Commit-Ansicht der WebUI (nur Anzeige,
 * docs/adr/10_ADR_WebUI.md). Myers-Algorithmus auf dem Teil zwischen
 * gemeinsamem Anfang und Ende; bei sehr vielen Aenderungen (MAX_D)
 * wird der Mittelteil als komplett ersetzt gezeigt statt die Seite
 * minutenlang rechnen zu lassen. */
class Diff
{
    const CONTEXT = 3;
    const MAX_D   = 1500;

    /* Text -> Zeilen ohne Zeilenende; ein abschliessender Zeilenumbruch
     * erzeugt keine leere Extrazeile. */
    public static function splitLines(string $text): array
    {
        if ($text === '') {
            return [];
        }
        $lines = preg_split("/\r\n|\n|\r/", $text);
        if (end($lines) === '') {
            array_pop($lines);
        }
        return $lines;
    }

    /* Liefert eine Liste von Operationen [Typ, Zeile alt, Zeile neu, Text]
     * mit Typ ' ' (gleich), '-' (entfernt), '+' (hinzugefuegt);
     * Zeilennummern 1-basiert, 0 = auf dieser Seite nicht vorhanden. */
    public static function ops(array $a, array $b): array
    {
        $n = count($a);
        $m = count($b);
        $pre = 0;
        while ($pre < $n && $pre < $m && $a[$pre] === $b[$pre]) {
            $pre++;
        }
        $suf = 0;
        while ($suf < $n - $pre && $suf < $m - $pre && $a[$n - 1 - $suf] === $b[$m - 1 - $suf]) {
            $suf++;
        }

        $ops = [];
        for ($i = 0; $i < $pre; $i++) {
            $ops[] = [' ', $i + 1, $i + 1, $a[$i]];
        }
        $midA = array_slice($a, $pre, $n - $pre - $suf);
        $midB = array_slice($b, $pre, $m - $pre - $suf);
        foreach (self::myers($midA, $midB) as [$t, $i, $j]) {
            $ops[] = $t === '+' ? ['+', 0, $pre + $j + 1, $midB[$j]]
                   : ($t === '-' ? ['-', $pre + $i + 1, 0, $midA[$i]]
                                 : [' ', $pre + $i + 1, $pre + $j + 1, $midA[$i]]);
        }
        for ($k = $suf; $k > 0; $k--) {
            $ops[] = [' ', $n - $k + 1, $m - $k + 1, $a[$n - $k]];
        }
        return $ops;
    }

    /* Klassischer Myers mit gespeicherten V-Zustaenden fuer den
     * Rueckweg. Liefert [Typ, Index alt, Index neu] (0-basiert). */
    private static function myers(array $a, array $b): array
    {
        $n = count($a);
        $m = count($b);
        if ($n === 0 || $m === 0) {
            $out = [];
            for ($i = 0; $i < $n; $i++) $out[] = ['-', $i, 0];
            for ($j = 0; $j < $m; $j++) $out[] = ['+', 0, $j];
            return $out;
        }
        $max   = $n + $m;
        $v     = [1 => 0];
        $trace = [];
        $found = false;
        for ($d = 0; $d <= $max && $d <= self::MAX_D; $d++) {
            $trace[] = $v;
            for ($k = -$d; $k <= $d; $k += 2) {
                if ($k === -$d || ($k !== $d && ($v[$k - 1] ?? -1) < ($v[$k + 1] ?? -1))) {
                    $x = $v[$k + 1] ?? 0;
                } else {
                    $x = ($v[$k - 1] ?? 0) + 1;
                }
                $y = $x - $k;
                while ($x < $n && $y < $m && $a[$x] === $b[$y]) {
                    $x++;
                    $y++;
                }
                $v[$k] = $x;
                if ($x >= $n && $y >= $m) {
                    $found = true;
                    break 2;
                }
            }
        }
        if (!$found) {
            /* Zu viele Unterschiede: alles alte raus, alles neue rein */
            $out = [];
            for ($i = 0; $i < $n; $i++) $out[] = ['-', $i, 0];
            for ($j = 0; $j < $m; $j++) $out[] = ['+', 0, $j];
            return $out;
        }

        $out = [];
        $x = $n;
        $y = $m;
        for ($d = count($trace) - 1; $d >= 0; $d--) {
            $vd = $trace[$d];
            $k  = $x - $y;
            if ($k === -$d || ($k !== $d && ($vd[$k - 1] ?? -1) < ($vd[$k + 1] ?? -1))) {
                $pk = $k + 1;
            } else {
                $pk = $k - 1;
            }
            $px = $vd[$pk] ?? 0;
            $py = $px - $pk;
            while ($x > $px && $y > $py) {
                $x--;
                $y--;
                $out[] = [' ', $x, $y];
            }
            if ($d > 0) {
                if ($x === $px) {
                    $y--;
                    $out[] = ['+', $x, $y];
                } else {
                    $x--;
                    $out[] = ['-', $x, $y];
                }
            }
        }
        return array_reverse($out);
    }

    /* Operationen -> Hunks mit CONTEXT Zeilen Umgebung. Jeder Hunk:
     * ['old_start','old_len','new_start','new_len','lines'=>[ops...]] */
    public static function hunks(array $ops): array
    {
        $count   = count($ops);
        $changed = [];
        foreach ($ops as $i => $op) {
            if ($op[0] !== ' ') {
                $changed[] = $i;
            }
        }
        if ($changed === []) {
            return [];
        }
        $ranges = [];
        foreach ($changed as $i) {
            $s = max(0, $i - self::CONTEXT);
            $e = min($count - 1, $i + self::CONTEXT);
            if ($ranges !== [] && $s <= $ranges[count($ranges) - 1][1] + 1) {
                $ranges[count($ranges) - 1][1] = max($ranges[count($ranges) - 1][1], $e);
            } else {
                $ranges[] = [$s, $e];
            }
        }
        $hunks = [];
        foreach ($ranges as [$s, $e]) {
            $lines = array_slice($ops, $s, $e - $s + 1);
            $oldStart = $newStart = 0;
            $oldLen = $newLen = 0;
            foreach ($lines as $op) {
                if ($op[0] !== '+') {
                    $oldLen++;
                    if ($oldStart === 0) $oldStart = $op[1];
                }
                if ($op[0] !== '-') {
                    $newLen++;
                    if ($newStart === 0) $newStart = $op[2];
                }
            }
            $hunks[] = ['old_start' => $oldStart, 'old_len' => $oldLen,
                        'new_start' => $newStart, 'new_len' => $newLen, 'lines' => $lines];
        }
        return $hunks;
    }
}
