<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Lesender Zugriff auf ein Repository fuer die WebUI: Objekte von der
 * Platte, Pfade ueber Tree-Eintraege, Historie, geaenderte Dateien, ZIP.
 * Nur Anzeige -- der Server entscheidet weiterhin nichts (ADR 09/10).
 * Pfade werden ausschliesslich ueber Tree-Eintraege aufgeloest, nie ueber
 * das Dateisystem; ein Name aus einem Tree kann daher nirgends hin
 * "ausbrechen". Alle Methoden vertragen kaputte/fehlende Objekte und
 * liefern dann null bzw. leere Listen. */
class RepoReader
{
    const TYPE_BLOB   = 0;
    const TYPE_TREE   = 1;
    const TYPE_COMMIT = 2;
    const MAX_DEPTH   = 64;
    /* Rechenbudget je Request: ein Tree darf mehrfach auf denselben
     * Untertree zeigen, die Knotenzahl waechst dann mit 2^Tiefe.
     * MAX_DEPTH allein begrenzt das nicht. */
    const MAX_TREE_VISITS = 20000;
    const MAX_CHANGED     = 5000;
    const MAX_ZIP_ENTRIES = 20000;
    const ZIP_SLOTS       = 2;

    private int $visits = 0;
    /* true, wenn ein Budget erschoepft wurde und Ergebnisse unvollstaendig sind */
    public bool $truncated = false;
    private $zipSlot = null;

    private array  $repo;
    private string $dir;
    private array  $types   = [];
    private array  $commits = [];
    private array  $trees   = [];

    public function __construct(array $repo)
    {
        $this->repo = $repo;
        $this->dir  = Repos::storageDir($repo['owner'], $repo['name']) . '/objects/';
    }

    public static function isOid(string $s): bool
    {
        return (bool)preg_match('/^[0-9a-f]{32}$/', $s);
    }

    /* Typen vieler Objekte mit einer Abfrage holen (Code-Ansicht) */
    public function prefetchTypes(array $oids): void
    {
        $oids = array_values(array_filter(array_unique($oids), function ($o) {
            return self::isOid($o) && !array_key_exists($o, $this->types);
        }));
        foreach (array_chunk($oids, 500) as $chunk) {
            $ph   = implode(',', array_fill(0, count($chunk), '?'));
            $stmt = DB::get()->prepare("SELECT oid, type FROM objects WHERE repo_id = ? AND oid IN ($ph)");
            $stmt->execute(array_merge([(int)$this->repo['id']], $chunk));
            foreach ($chunk as $o) {
                $this->types[$o] = null;
            }
            foreach ($stmt->fetchAll() as $r) {
                $this->types[$r['oid']] = (int)$r['type'];
            }
        }
    }

    private function spend(): bool
    {
        if (++$this->visits > self::MAX_TREE_VISITS) {
            $this->truncated = true;
            return false;
        }
        return true;
    }

    /* Darf ein Tree-Name als Pfad in ein ZIP? Kein "."/"..", keine
     * Trenner (/ \ und Amiga ":"), keine Steuerzeichen -- sonst koennte
     * ein Entpacker ausserhalb des Zielordners schreiben (ZIP-Slip). */
    public static function safeName(string $name): bool
    {
        return $name !== '' && $name !== '.' && $name !== '..'
            && !preg_match('#[/\\\\:\x00-\x1f\x7f]#', $name);
    }

    public function type(string $oid): ?int
    {
        if (!array_key_exists($oid, $this->types)) {
            $stmt = DB::get()->prepare('SELECT type FROM objects WHERE repo_id = ? AND oid = ?');
            $stmt->execute([(int)$this->repo['id'], $oid]);
            $t = $stmt->fetchColumn();
            $this->types[$oid] = $t === false ? null : (int)$t;
        }
        return $this->types[$oid];
    }

    public function bytes(string $oid, int $expectType): ?string
    {
        if (!self::isOid($oid) || $this->type($oid) !== $expectType) {
            return null;
        }
        $data = @file_get_contents($this->dir . substr($oid, 0, 2) . '/' . substr($oid, 2));
        return $data === false ? null : $data;
    }

    public function blobPath(string $oid): ?string
    {
        if (!self::isOid($oid) || $this->type($oid) !== self::TYPE_BLOB) {
            return null;
        }
        $p = $this->dir . substr($oid, 0, 2) . '/' . substr($oid, 2);
        return is_file($p) ? $p : null;
    }

    public function blobSize(string $oid): int
    {
        $p = $this->blobPath($oid);
        return $p === null ? 0 : (int)filesize($p);
    }

    public function commit(string $oid): ?array
    {
        if (!array_key_exists($oid, $this->commits)) {
            $data = $this->bytes($oid, self::TYPE_COMMIT);
            try {
                $this->commits[$oid] = $data === null ? null : ObjectDecoder::decodeCommit($data);
            } catch (RuntimeException $e) {
                $this->commits[$oid] = null;
            }
        }
        return $this->commits[$oid];
    }

    /* Eintraege sortiert: Ordner zuerst, dann natuerliche Namensfolge */
    public function tree(string $oid): ?array
    {
        if (!array_key_exists($oid, $this->trees)) {
            $data = $this->bytes($oid, self::TYPE_TREE);
            try {
                $entries = $data === null ? null : ObjectDecoder::decodeTree($data);
            } catch (RuntimeException $e) {
                $entries = null;
            }
            if ($entries !== null) {
                usort($entries, function (array $a, array $b): int {
                    if ($a['is_dir'] !== $b['is_dir']) {
                        return $a['is_dir'] ? -1 : 1;
                    }
                    return strnatcasecmp($a['name'], $b['name']);
                });
            }
            $this->trees[$oid] = $entries;
        }
        return $this->trees[$oid];
    }

    /* ── Refs ──────────────────────────────────────────────────────────── */

    /* Branches, Standard-Branch zuerst: [['name'=>refs/heads/x,'short'=>x,'oid'=>…]] */
    public function branches(): array
    {
        $stmt = DB::get()->prepare('SELECT name, oid FROM refs WHERE repo_id = ? AND oid <> \'\' ORDER BY name');
        $stmt->execute([(int)$this->repo['id']]);
        $out = [];
        foreach ($stmt->fetchAll() as $r) {
            $r['short'] = preg_replace('#^refs/heads/#', '', $r['name']);
            $out[] = $r;
        }
        $def = $this->repo['default_branch'];
        usort($out, function (array $a, array $b) use ($def): int {
            if (($a['name'] === $def) !== ($b['name'] === $def)) {
                return $a['name'] === $def ? -1 : 1;
            }
            return strnatcasecmp($a['short'], $b['short']);
        });
        return $out;
    }

    /* Standard-Branch, sonst "main", sonst der erste vorhandene */
    public function defaultBranch(): ?array
    {
        $all = $this->branches();
        foreach ($all as $b) {
            if ($b['name'] === $this->repo['default_branch']) return $b;
        }
        foreach ($all as $b) {
            if ($b['short'] === 'main') return $b;
        }
        return $all[0] ?? null;
    }

    /* Branchname (kurz) oder Commit-OID -> Commit-OID */
    public function resolve(string $ref): ?string
    {
        if (self::isOid($ref)) {
            return $this->commit($ref) !== null ? $ref : null;
        }
        foreach ($this->branches() as $b) {
            if ($b['short'] === $ref) {
                return $b['oid'];
            }
        }
        return null;
    }

    /* ── Pfade ─────────────────────────────────────────────────────────── */

    /* Pfad im Commit -> ['type'=>'tree'|'blob','oid'=>…,'name'=>…] oder null.
     * "" = Wurzel. Segmente muessen exakt einem Tree-Eintrag entsprechen. */
    public function lookup(string $commitOid, string $path): ?array
    {
        $c = $this->commit($commitOid);
        if ($c === null) {
            return null;
        }
        $node = ['type' => 'tree', 'oid' => $c['tree'], 'name' => ''];
        $parts = $path === '' ? [] : explode('/', $path);
        if (count($parts) > self::MAX_DEPTH) {
            return null;
        }
        foreach ($parts as $seg) {
            if ($node['type'] !== 'tree' || $seg === '') {
                return null;
            }
            $entries = $this->tree($node['oid']);
            if ($entries === null) {
                return null;
            }
            $next = null;
            foreach ($entries as $e) {
                if ($e['name'] === $seg) {
                    $next = ['type' => $e['is_dir'] ? 'tree' : 'blob', 'oid' => $e['oid'], 'name' => $seg];
                    break;
                }
            }
            if ($next === null) {
                return null;
            }
            $node = $next;
        }
        return $node;
    }

    /* ── Historie ──────────────────────────────────────────────────────── */

    /* Graph ueber die angegebenen Refs (Liste ['name','oid']), hoechstens
     * $limit Commits (neueste zuerst). */
    public function history(array $refs, int $limit): array
    {
        return GraphLayout::build($refs, function (string $oid): ?array {
            return $this->commit($oid);
        }, $limit);
    }

    /* ── Geaenderte Dateien eines Commits ──────────────────────────────── */

    /* Vergleich mit dem ersten Elternteil (Merge-Commits wie
     * "git show --first-parent"). Liste ['path','status'=>A|D|M,'old','new']. */
    public function changedFiles(string $commitOid): array
    {
        $c = $this->commit($commitOid);
        if ($c === null) {
            return [];
        }
        $oldTree = null;
        if ($c['parents'] !== []) {
            $p = $this->commit($c['parents'][0]);
            $oldTree = $p === null ? null : $p['tree'];
        }
        $out = [];
        $this->diffTrees($oldTree, $c['tree'], '', $out, 0);
        return $out;
    }

    private function diffTrees(?string $old, ?string $new, string $prefix, array &$out, int $depth): void
    {
        if ($old === $new || $depth > self::MAX_DEPTH) {
            return;
        }
        if (!$this->spend() || count($out) >= self::MAX_CHANGED) {
            $this->truncated = true;
            return;
        }
        $a = [];
        foreach (($old === null ? [] : ($this->tree($old) ?? [])) as $e) {
            $a[$e['name']] = $e;
        }
        $b = [];
        foreach (($new === null ? [] : ($this->tree($new) ?? [])) as $e) {
            $b[$e['name']] = $e;
        }
        $names = array_unique(array_merge(array_keys($a), array_keys($b)));
        usort($names, 'strnatcasecmp');
        foreach ($names as $name) {
            $name = (string)$name;
            $ea = $a[$name] ?? null;
            $eb = $b[$name] ?? null;
            $path = $prefix . $name;
            if ($ea !== null && $eb !== null && $ea['oid'] === $eb['oid'] && $ea['is_dir'] === $eb['is_dir']) {
                continue;
            }
            $aDir = $ea !== null && $ea['is_dir'];
            $bDir = $eb !== null && $eb['is_dir'];
            if ($aDir || $bDir) {
                /* Ordner rekursiv; Typwechsel Datei<->Ordner = Loeschen + Anlegen */
                if ($ea !== null && !$aDir) {
                    $out[] = ['path' => $path, 'status' => 'D', 'old' => $ea['oid'], 'new' => null];
                }
                if ($eb !== null && !$bDir) {
                    $out[] = ['path' => $path, 'status' => 'A', 'old' => null, 'new' => $eb['oid']];
                }
                $this->diffTrees($aDir ? $ea['oid'] : null, $bDir ? $eb['oid'] : null, $path . '/', $out, $depth + 1);
                continue;
            }
            $status = $ea === null ? 'A' : ($eb === null ? 'D' : 'M');
            $out[] = ['path' => $path, 'status' => $status,
                      'old' => $ea === null ? null : $ea['oid'], 'new' => $eb === null ? null : $eb['oid']];
        }
    }

    /* ── Inhalt als Text ───────────────────────────────────────────────── */

    public static function isBinary(string $data): bool
    {
        return strpos(substr($data, 0, 8000), "\0") !== false;
    }

    /* Amiga-Textdateien sind meist ISO-8859-1: ist der Inhalt kein
     * gueltiges UTF-8, fuer die Anzeige als Latin-1 lesen. Liefert
     * [Text in UTF-8, true wenn umgewandelt]. */
    public static function toUtf8(string $data): array
    {
        if (mb_check_encoding($data, 'UTF-8')) {
            return [$data, false];
        }
        return [mb_convert_encoding($data, 'UTF-8', 'ISO-8859-1'), true];
    }

    /* ── ZIP ───────────────────────────────────────────────────────────── */

    /* Tree (rekursiv) als ZIP-Datei; liefert den Pfad der Temp-Datei
     * oder eine Fehlermeldung (string beginnt nicht mit "/"). */
    public function zip(string $treeOid, string $rootName)
    {
        if (!class_exists('ZipArchive')) {
            return 'ZIP-Download ist auf diesem Server nicht verfügbar (PHP-Erweiterung zip fehlt).';
        }
        /* Hoechstens ZIP_SLOTS gleichzeitige ZIP-Erstellungen (CPU, /tmp) */
        for ($i = 0; $i < self::ZIP_SLOTS && $this->zipSlot === null; $i++) {
            $fp = @fopen(sys_get_temp_dir() . '/amivcs-zip-' . $i . '.lock', 'c');
            if ($fp && flock($fp, LOCK_EX | LOCK_NB)) {
                $this->zipSlot = $fp;
            } elseif ($fp) {
                fclose($fp);
            }
        }
        if ($this->zipSlot === null) {
            return 'Gerade werden zu viele ZIP-Dateien erstellt. Bitte gleich noch einmal versuchen.';
        }
        $tmp = tempnam(sys_get_temp_dir(), 'amivcszip');
        register_shutdown_function(function () use ($tmp) {
            @unlink($tmp);
        });
        $zip = new ZipArchive();
        if ($zip->open($tmp, ZipArchive::OVERWRITE) !== true) {
            return 'ZIP-Datei konnte nicht erstellt werden.';
        }
        $total = 0;
        $count = 0;
        $ok = $this->zipTree($zip, $treeOid, $rootName . '/', $total, $count, 0);
        $zip->close();
        if (!$ok) {
            return 'Das Repository ist zu groß für einen ZIP-Download (über 1 GB oder '
                 . self::MAX_ZIP_ENTRIES . ' Einträge) – bitte mit amivcs clone holen.';
        }
        return $tmp;
    }

    private function zipTree(ZipArchive $zip, string $treeOid, string $prefix, int &$total, int &$count, int $depth): bool
    {
        if ($depth > self::MAX_DEPTH) {
            return true;
        }
        if (!$this->spend() || ++$count > self::MAX_ZIP_ENTRIES) {
            return false;
        }
        $zip->addEmptyDir(rtrim($prefix, '/'));
        foreach ($this->tree($treeOid) ?? [] as $e) {
            if (!self::safeName($e['name'])) {
                continue;
            }
            if ($e['is_dir']) {
                if (!$this->zipTree($zip, $e['oid'], $prefix . $e['name'] . '/', $total, $count, $depth + 1)) {
                    return false;
                }
                continue;
            }
            if (++$count > self::MAX_ZIP_ENTRIES) {
                return false;
            }
            $p = $this->blobPath($e['oid']);
            if ($p === null) {
                continue;
            }
            $total += (int)filesize($p);
            if ($total > 1073741824) {
                return false;
            }
            $zip->addFile($p, $prefix . $e['name']);
            $zip->setCompressionName($prefix . $e['name'], ZipArchive::CM_STORE);
        }
        return true;
    }
}
