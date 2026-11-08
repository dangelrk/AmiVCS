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
# globale Vorgaben in einem eigenen HOME (~/.amivcsconfig), nicht im echten
export HOME="$TMP/home"
mkdir -p "$HOME"
printf 'name=Test Person\nemail=test@example.org\nverify_ssl=0\n' > "$HOME/.amivcsconfig"
URL="${AC_SERVER%/}/$REPO"
LOGIN="--user $AC_USER --password $AC_PASS"
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
t "init"                       at "$A" "$VCS" init
t "commit -a"                  at "$A" "$VCS" commit -a -m "erster Stand"
t "Autor aus globaler Vorgabe" sh -c "cd '$A' && '$VCS' log | grep -q 'Test Person <test@example.org>'"
tnot "push ohne Remote"        at "$A" "$VCS" push $LOGIN
tnot "push ohne Passwort"      at "$A" "$VCS" push "$URL" --user "$AC_USER"
tnot "push mit falschem Passwort" at "$A" "$VCS" push "$URL" --user "$AC_USER" --password falsch
t "push mit URL (legt Repo an)" at "$A" "$VCS" push "$URL" $LOGIN
t "Remote gemerkt"             sh -c "cd '$A' && test \"\$('$VCS' remote)\" = '$URL'"
t "Benutzer gemerkt, Passwort nicht" sh -c "grep -q '^user=$AC_USER\$' '$A/.amivcs/config' && ! grep -q '^pass=' '$A/.amivcs/config'"
t "refs zeigt den Branch"      json "action=refs&repo=$REPO" "'refs/heads/main' in json.dumps(j)"

echo "== Klonen"
B="$TMP/b"
t "clone (Verzeichnis = Name)" at "$TMP" "$VCS" clone "$URL" $LOGIN --remember-password
t "Klon liegt in $REPO"        test -d "$TMP/$REPO/.amivcs"
mv "$TMP/$REPO" "$B"
for f in readme.txt sub/liste.txt sub/binaer.dat; do
    t "geklont gleich: $f"     same "$A/$f" "$B/$f"
done
t "Klon kennt Remote und Passwort" sh -c "grep -q '^remote=$URL\$' '$B/.amivcs/config' && grep -q '^pass=' '$B/.amivcs/config'"
t "config zeigt pass verdeckt" sh -c "cd '$B' && '$VCS' config | grep -q '^pass=\*\*\*\*'"

echo "== Aenderung hin und zurueck"
printf 'drei\n' >> "$A/sub/liste.txt"
printf 'neu\n' > "$A/neu.txt"
t "commit Aenderung"           at "$A" "$VCS" commit -a -m "zweiter Stand"
t "push Aenderung"             at "$A" "$VCS" push $LOGIN
t "pull im Klon (gemerktes Passwort)" at "$B" "$VCS" pull
t "gepullt: liste.txt"         same "$A/sub/liste.txt" "$B/sub/liste.txt"
t "gepullt: neu.txt"           same "$A/neu.txt" "$B/neu.txt"
t "log im Klon hat beide Commits" sh -c "cd '$B' && '$VCS' log | grep -q 'zweiter Stand' && '$VCS' log | grep -q 'erster Stand'"

echo "== Identitaet pro Repository"
t "name nur fuer b"            at "$B" "$VCS" config name "Klon Person"
printf 'aus b\n' > "$B/anders.txt"
t "commit in b"                at "$B" "$VCS" commit -a -m "b hinten"
t "Autor aus dem Repository"   sh -c "cd '$B' && '$VCS' log | grep -q 'Klon Person <test@example.org>'"

echo "== Veralteter Push wird abgelehnt"
printf 'aus a\n' > "$A/konflikt.txt"
t "commit in a"                at "$A" "$VCS" commit -a -m "a vorn"
t "push aus a"                 at "$A" "$VCS" push $LOGIN
tnot "push aus b ohne pull"    at "$B" "$VCS" push
t "pull in b fuehrt zusammen"  at "$B" "$VCS" pull
t "push aus b nach pull"       at "$B" "$VCS" push
t "pull in a"                  at "$A" "$VCS" pull $LOGIN
t "a hat anders.txt"           same "$A/anders.txt" "$B/anders.txt"

echo "== Zweiter Branch"
t "branch dev"                 at "$A" "$VCS" branch dev
t "switch dev"                 at "$A" "$VCS" switch dev
printf 'dev\n' > "$A/dev.txt"
t "commit auf dev"             at "$A" "$VCS" commit -a -m "auf dev"
t "push dev"                   at "$A" "$VCS" push $LOGIN
t "refs zeigt dev"             json "action=refs&repo=$REPO" "'refs/heads/dev' in json.dumps(j)"

echo "== Passwort vergessen, ohne Namen kein Commit"
t "config --unset pass"        at "$B" "$VCS" config --unset pass
tnot "pull ohne Passwort"      at "$B" "$VCS" pull
rm "$HOME/.amivcsconfig"
C="$TMP/c"; mkdir -p "$C"; printf 'x\n' > "$C/x"
t "init ohne Identitaet"       at "$C" "$VCS" init
tnot "commit ohne Namen"       at "$C" "$VCS" commit -a -m "anonym"
t "config --global name"       at "$C" "$VCS" config --global name "Neu"
t "commit mit globalem Namen"  at "$C" "$VCS" commit -a -m "jetzt"

echo "== Remote vorher gesetzt, Repository fehlt noch"
t "remote setzen"              at "$C" "$VCS" remote "$URL-2"
t "push legt das Repository an" at "$C" "$VCS" push $LOGIN
t "refs des neuen Repositorys" json "action=refs&repo=$REPO-2" "'refs/heads/main' in json.dumps(j)"

echo "== Ergebnis: $PASSED ok, $FAILED fehlgeschlagen =="
[ "$FAILED" -eq 0 ]
