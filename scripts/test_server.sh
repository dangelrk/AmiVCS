#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# AmiVCS-Server-Test: Rundlauf mit der PC-CLI (bin/pc/amivcs) gegen einen
# laufenden Server -- init/commit/push, clone, Aenderung pushen und pullen,
# zweiter Branch, Schutz vor veraltetem Push, dazu Rohabfragen an api.php
# und web.php. Laeuft auch in make server-test (core/tools/servertest.py).
#
# Aufruf:  AC_SERVER=http://127.0.0.1:8098 AC_USER=bob AC_PASS=... \
#          scripts/test_server.sh
# Braucht: make pc (bin/pc/amivcs), curl, python3. Legt das Repository
# pkgtest-<pid> an; der Server hat kein Loeschen per API, es bleibt stehen
# (in make server-test verschwindet es mit der Wegwerf-Datenbank).

set -u
: "${AC_SERVER:?AC_SERVER fehlt}"
: "${AC_USER:?AC_USER fehlt}"
: "${AC_PASS:?AC_PASS fehlt}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VCS="$ROOT/bin/pc/amivcs"
[ -x "$VCS" ] || { echo "$VCS fehlt (make pc)" >&2; exit 1; }
API="${AC_SERVER%/}/api.php"
REPO="pkgtest-$$"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CFG="$TMP/amivcs.cfg"
printf 'server=%s\nuser=%s\npass=%s\nverify_ssl=0\n' "${AC_SERVER%/}" "$AC_USER" "$AC_PASS" > "$CFG"
PASSED=0
FAILED=0

ok()   { PASSED=$((PASSED + 1)); echo "ok    $1"; }
bad()  { FAILED=$((FAILED + 1)); echo "FEHLER $1"; }
t()    { name=$1; shift; if "$@" >"$TMP/out" 2>&1; then ok "$name"; else bad "$name"; sed 's/^/      /' "$TMP/out"; fi; }
tnot() { name=$1; shift; if "$@" >"$TMP/out" 2>&1; then bad "$name (unerwartet erfolgreich)"; else ok "$name"; fi; }
at()   { d=$1; shift; (cd "$d" && "$@"); }
same() { cmp -s "$1" "$2"; }
json() { curl -sS -u "$AC_USER:$AC_PASS" "$API?$1" | python3 -c "import json,sys; j=json.load(sys.stdin); sys.exit(0 if ($2) else 1)"; }

echo "== Server"
t "status mit Anmeldung"       json "action=status" "j.get('ok') is not False"
tnot "status ohne Anmeldung"   curl -sSf "$API?action=status"
tnot "falsches Passwort"       curl -sSf -u "$AC_USER:falsch" "$API?action=status"
t "web.php: Startseite (Entdecken)" sh -c "curl -sSf '${AC_SERVER%/}/web.php' >/dev/null"
t "web.php: Anmeldeseite"      sh -c "curl -sSf '${AC_SERVER%/}/web.php?view=login' | grep -q 'type=\"password\"'"

echo "== Anlegen und pushen"
A="$TMP/a"
mkdir -p "$A/sub"
printf 'Hallo AmiVCS\n' > "$A/readme.txt"
printf 'eins\nzwei\n' > "$A/sub/liste.txt"
head -c 300000 /dev/urandom > "$A/sub/binaer.dat"
t "init"                       at "$A" "$VCS" -c "$CFG" init
t "commit -a"                  at "$A" "$VCS" -c "$CFG" commit -a -m "erster Stand"
t "push (legt Repo an)"        at "$A" "$VCS" -c "$CFG" push "$REPO"
t "refs zeigt den Branch"      json "action=refs&repo=$REPO" "'refs/heads/main' in json.dumps(j)"

echo "== Klonen"
B="$TMP/b"
t "clone"                      at "$TMP" "$VCS" -c "$CFG" clone "$REPO" b
for f in readme.txt sub/liste.txt sub/binaer.dat; do
    t "geklont gleich: $f"     same "$A/$f" "$B/$f"
done

echo "== Aenderung hin und zurueck"
printf 'drei\n' >> "$A/sub/liste.txt"
printf 'neu\n' > "$A/neu.txt"
t "commit Aenderung"           at "$A" "$VCS" -c "$CFG" commit -a -m "zweiter Stand"
t "push Aenderung"             at "$A" "$VCS" -c "$CFG" push
t "pull im Klon"               at "$B" "$VCS" -c "$CFG" pull
t "gepullt: liste.txt"         same "$A/sub/liste.txt" "$B/sub/liste.txt"
t "gepullt: neu.txt"           same "$A/neu.txt" "$B/neu.txt"
t "log im Klon hat beide Commits" sh -c "cd '$B' && '$VCS' -c '$CFG' log | grep -q 'zweiter Stand' && '$VCS' -c '$CFG' log | grep -q 'erster Stand'"

echo "== Veralteter Push wird abgelehnt"
printf 'aus a\n' > "$A/konflikt.txt"
t "commit in a"                at "$A" "$VCS" -c "$CFG" commit -a -m "a vorn"
t "push aus a"                 at "$A" "$VCS" -c "$CFG" push
printf 'aus b\n' > "$B/anders.txt"
t "commit in b"                at "$B" "$VCS" -c "$CFG" commit -a -m "b hinten"
tnot "push aus b ohne pull"    at "$B" "$VCS" -c "$CFG" push
t "pull in b fuehrt zusammen"  at "$B" "$VCS" -c "$CFG" pull
t "push aus b nach pull"       at "$B" "$VCS" -c "$CFG" push
t "pull in a"                  at "$A" "$VCS" -c "$CFG" pull
t "a hat anders.txt"           same "$A/anders.txt" "$B/anders.txt"

echo "== Zweiter Branch"
t "branch dev"                 at "$A" "$VCS" -c "$CFG" branch dev
t "switch dev"                 at "$A" "$VCS" -c "$CFG" switch dev
printf 'dev\n' > "$A/dev.txt"
t "commit auf dev"             at "$A" "$VCS" -c "$CFG" commit -a -m "auf dev"
t "push dev"                   at "$A" "$VCS" -c "$CFG" push
t "refs zeigt dev"             json "action=refs&repo=$REPO" "'refs/heads/dev' in json.dumps(j)"

echo "== Ergebnis: $PASSED ok, $FAILED fehlgeschlagen =="
[ "$FAILED" -eq 0 ]
