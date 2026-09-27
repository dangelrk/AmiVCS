#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# run-fsuae.sh -- kopiert gebaute AmigaOS-Programme aus bin/amiga/ in einen
# Austauschordner und startet FS-UAE mit einer vorhandenen Konfiguration.
#
# Aufruf:   scripts/run-fsuae.sh [programm ...]
#   ohne Argumente: ALLE Programme aus bin/amiga/ werden kopiert
#   mit Argumenten: nur die genannten (z.B. "run-fsuae.sh amivcs amivcs-gui")
#
# Voraussetzung: die Programme sind gebaut ("make amiga", siehe
# docs/BUILD.md).
#
# Konfiguration ueber Umgebungsvariablen:
#   FSUAE_BIN     FS-UAE-Programm. Standard: "fs-uae" aus dem PATH, unter
#                 macOS ersatzweise /Applications/FS-UAE.app/Contents/MacOS/fs-uae
#   FSUAE_CONFIG  FS-UAE-Konfigurationsdatei (*.fs-uae) -- muss gesetzt sein
#   FSUAE_SHARED  Host-Ordner, in den kopiert wird -- muss gesetzt sein.
#                 Die FS-UAE-Konfiguration muss genau diesen Ordner als
#                 Laufwerk einbinden (z.B. hard_drive_1 mit Label SHARED);
#                 im Amiga-Shell dann z.B.:  CD SHARED:  und  amivcs version

set -eu

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
AMIGA_BIN_DIR="$PROJECT_DIR/bin/amiga"

if [ -z "${FSUAE_BIN:-}" ]; then
    if command -v fs-uae >/dev/null 2>&1; then
        FSUAE_BIN="$(command -v fs-uae)"
    elif [ -x /Applications/FS-UAE.app/Contents/MacOS/fs-uae ]; then
        FSUAE_BIN=/Applications/FS-UAE.app/Contents/MacOS/fs-uae
    else
        echo "FS-UAE nicht gefunden -- bitte FSUAE_BIN setzen." >&2
        exit 1
    fi
fi
[ -x "$FSUAE_BIN" ] || { echo "FSUAE_BIN ist nicht ausfuehrbar: $FSUAE_BIN" >&2; exit 1; }

[ -n "${FSUAE_CONFIG:-}" ] || { echo "Bitte FSUAE_CONFIG auf die FS-UAE-Konfiguration (*.fs-uae) setzen." >&2; exit 1; }
[ -f "$FSUAE_CONFIG" ] || { echo "FS-UAE-Konfiguration nicht gefunden: $FSUAE_CONFIG" >&2; exit 1; }

[ -n "${FSUAE_SHARED:-}" ] || { echo "Bitte FSUAE_SHARED auf den Austauschordner setzen, den die FS-UAE-Konfiguration einbindet." >&2; exit 1; }
mkdir -p "$FSUAE_SHARED"

if [ "$#" -gt 0 ]; then
    for name in "$@"; do
        src="$AMIGA_BIN_DIR/$name"
        [ -f "$src" ] || { echo "Nicht gefunden: $src (erst \"make amiga\" ausfuehren)" >&2; exit 1; }
        cp "$src" "$FSUAE_SHARED/"
        echo "Kopiert: $src -> $FSUAE_SHARED/$name"
    done
else
    [ -d "$AMIGA_BIN_DIR" ] && [ -n "$(ls -A "$AMIGA_BIN_DIR" 2>/dev/null)" ] || { echo "Keine Programme in $AMIGA_BIN_DIR gefunden (erst \"make amiga\" ausfuehren)." >&2; exit 1; }
    for src in "$AMIGA_BIN_DIR"/*; do
        [ -f "$src" ] || continue
        cp "$src" "$FSUAE_SHARED/"
        echo "Kopiert: $src -> $FSUAE_SHARED/$(basename "$src")"
    done
fi

echo "Starte FS-UAE ($FSUAE_CONFIG) ..."
exec "$FSUAE_BIN" "$FSUAE_CONFIG"
