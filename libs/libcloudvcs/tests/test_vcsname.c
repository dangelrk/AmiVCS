/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/* Host unit tests for vcsname.c. The "local" codeset is switched with
 * charset.h's cs_test_mode(): ISO-8859-1 plays the Amiga, the default
 * the PC. */
#include "vcsname.h"
#include "charset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_ok, g_fail;
#define CHECK(c, what) do { if (c) g_ok++; else { g_fail++; printf("FEHLER: %s (Zeile %d)\n", what, __LINE__); } } while (0)
#define SAME(a, b, what) CHECK(strcmp((a), (b)) == 0, what)

#define U_GROESSE "Gr\xC3\xB6\xC3\x9F" "e.txt"
#define L_GROESSE "Gr\xF6\xDF" "e.txt"
#define U_ORDNER  "\xC3\x84nderungen"
#define L_ORDNER  "\xC4nderungen"
#define U_EURO    "Preis\xE2\x82\xAC.txt"
#define U_POUND   "Preis\xE2\x82\xA4.txt"
#define NFD_GROESSE "Gro\xCC\x88\xC3\x9F" "e.txt"

static int settled_x(const char *repo, void *ctx) { (void)ctx; return strncmp(repo, "x/", 2) == 0; }
static void from(const char *l, char *out) { vcs_path_from_local(l, out, 256); }

int main(void)
{
    char out[256], back[256], a[256], b[256];
    char *t;

    /* ---------------- Amiga (ISO-8859-1) ---------------- */
    cs_test_mode(CS_TEST_LATIN1);
    vcs_names_reset();

    CHECK(vcs_path_to_local(U_ORDNER "/" U_GROESSE, out, sizeof(out)) == 1, "UTF-8 -> Latin-1 exakt");
    SAME(out, L_ORDNER "/" L_GROESSE, "Pfad lokal");
    from(L_ORDNER "/" L_GROESSE, back);
    SAME(back, U_ORDNER "/" U_GROESSE, "Pfad zurueck nach UTF-8");

    /* a character ISO-8859-1 lacks: written as _, mapped back once learned */
    CHECK(vcs_path_to_local(U_ORDNER "/" U_EURO, out, sizeof(out)) == 0, "EUR nicht exakt");
    SAME(out, L_ORDNER "/Preis_.txt", "EUR als _");
    from(L_ORDNER "/Preis_.txt", back);
    SAME(back, U_ORDNER "/Preis_.txt", "ungelernt: wie auf der Platte");
    vcs_names_learn(U_ORDNER "/" U_EURO);
    from(L_ORDNER "/Preis_.txt", back);
    SAME(back, U_ORDNER "/" U_EURO, "gelernt: echter Name");
    from("Preis_.txt", back);
    SAME(back, "Preis_.txt", "gleicher Name in anderem Ordner bleibt");

    /* a new operation forgets everything (another repository) */
    vcs_names_reset();
    from(L_ORDNER "/Preis_.txt", back);
    SAME(back, U_ORDNER "/Preis_.txt", "nach reset nichts Altes");

    /* an exact name overrides an older pair */
    vcs_names_learn(U_EURO);
    vcs_names_learn("Preis_.txt");
    from("Preis_.txt", back);
    SAME(back, "Preis_.txt", "exakter Name gewinnt");

    /* collisions */
    {
        const char *p1[] = { U_EURO, "Preis_.txt" };
        const char *p2[] = { U_EURO, U_POUND };
        const char *p3[] = { "\xCE\xA9" "dir/a.txt", "_dir/b.txt" };
        const char *p4[] = { U_ORDNER "/" U_GROESSE, "x/" U_EURO, "y/" U_EURO };
        vcs_names_reset();
        CHECK(vcs_names_check(p1, 2, a, b, sizeof(a)) == 0, "Kollision mit echtem Namen");
        CHECK(vcs_names_check(p2, 2, a, b, sizeof(a)) == 0, "Kollision zweier Ersatznamen");
        CHECK(vcs_names_check(p3, 2, a, b, sizeof(a)) == 0, "Kollision ueber Ordner");
        CHECK(vcs_names_check(p4, 3, a, b, sizeof(a)) == 1, "keine Kollision");
    }

    /* older repository from an Amiga build up to 0.5: bytes stay, any codeset */
    vcs_names_reset();
    CHECK(vcs_path_to_local(L_GROESSE, out, sizeof(out)) == 0, "Altname braucht ein Paar");
    SAME(out, L_GROESSE, "Altname: die Bytes auf der Platte");
    vcs_names_learn(L_ORDNER "/a.txt");
    vcs_names_learn(L_GROESSE);
    from(L_GROESSE, back);
    SAME(back, L_GROESSE, "Altname bleibt im Repository");
    from(L_ORDNER "/a.txt", back);
    SAME(back, L_ORDNER "/a.txt", "Datei unter Alt-Ordner");
    from(L_ORDNER "/neu.txt", back);
    SAME(back, L_ORDNER "/neu.txt", "neue Datei unter Alt-Ordner haengt am Alt-Ordner");

    /* table file */
    {
        char dir[] = "/tmp/vcsnameXXXXXX";
        CHECK(mkdtemp(dir) != NULL, "mkdtemp");
        vcs_names_reset();
        vcs_names_learn("x/" U_EURO);
        vcs_names_save(dir);
        vcs_names_reset();
        vcs_names_load(dir);
        from("x/Preis_.txt", back);
        SAME(back, "x/" U_EURO, "Paar aus localnames");
        /* prune: a settled pair goes, another stays */
        vcs_names_reset();
        vcs_names_learn("x/" U_EURO);
        vcs_names_learn("y/" U_POUND);
        vcs_names_save(dir);
        vcs_names_prune(dir, settled_x, NULL);
        vcs_names_reset();
        vcs_names_load(dir);
        from("x/Preis_.txt", back);
        SAME(back, "x/Preis_.txt", "prune: erledigtes Paar weg");
        from("y/Preis_.txt", back);
        SAME(back, "y/" U_POUND, "prune: offenes Paar bleibt");
        vcs_names_forget(dir);
        vcs_names_reset();
        vcs_names_load(dir);
        from("x/Preis_.txt", back);
        SAME(back, "x/Preis_.txt", "localnames vergessen");
        vcs_names_reset();
        vcs_names_save(dir);       /* no pairs: no file */
        snprintf(out, sizeof(out), "%s/localnames", dir);
        CHECK(access(out, F_OK) != 0, "leere Tabelle: keine Datei");
        rmdir(dir);
    }

    /* texts */
    t = vcs_text_from_local("Gr\xFC\xDF" "e"); SAME(t, "Gr\xC3\xBC\xC3\x9F" "e", "Text nach UTF-8"); free(t);
    t = vcs_text_to_local("Gr\xC3\xBC\xC3\x9F" "e\nZeile 2?"); SAME(t, "Gr\xFC\xDF" "e\nZeile 2?", "Text lokal, ? bleibt"); free(t);
    t = vcs_text_to_local("Gr\xFC\xDF" "e"); SAME(t, "Gr\xFC\xDF" "e", "Alt-Text lokal unveraendert"); free(t);
    t = vcs_text_utf8("Gr\xFC\xDF" "e"); SAME(t, "Gr\xC3\xBC\xC3\x9F" "e", "Alt-Text als UTF-8"); free(t);
    {
        /* long message: no 1 KB limit */
        char *big = (char *)malloc(5000), *u, *l;
        int i;
        for (i = 0; i < 2000; i++) { big[2 * i] = (char)0xC3; big[2 * i + 1] = (char)0xA4; }
        strcpy(big + 4000, " Ende?");
        l = vcs_text_to_local(big);
        CHECK(l && strlen(l) == 2006 && (unsigned char)l[0] == 0xE4 && strcmp(l + 2000, " Ende?") == 0,
              "langer Text ganz");
        u = vcs_text_from_local(l);
        SAME(u, big, "langer Text zurueck");
        free(big); free(l); free(u);
    }
    vcs_utf8_copy(out, 4, "a\xC3\xA4\xC3\xB6");
    SAME(out, "a\xC3\xA4", "an Zeichengrenze gekuerzt");
    vcs_utf8_copy(out, 3, "a\xC3\xA4");
    SAME(out, "a", "kein halbes Zeichen");
    CHECK(vcs_utf8_valid("abc") && vcs_utf8_valid(U_EURO), "gueltig");
    CHECK(!vcs_utf8_valid(L_GROESSE) && !vcs_utf8_valid("\xC0\x80") && !vcs_utf8_valid("\xED\xA0\x80"),
          "ungueltig (Latin-1, ueberlange Form, Surrogat)");

    /* ---------------- PC (UTF-8) ---------------- */
    cs_test_mode(CS_TEST_DEFAULT);
    vcs_names_reset();
    CHECK(vcs_path_to_local(U_ORDNER "/" U_GROESSE, out, sizeof(out)) == 1, "PC: exakt");
    SAME(out, U_ORDNER "/" U_GROESSE, "PC: unveraendert");
    /* an older Amiga name: written as UTF-8, mapped back to its bytes */
    CHECK(vcs_path_to_local(L_GROESSE, out, sizeof(out)) == 0, "PC: Altname braucht ein Paar");
    SAME(out, U_GROESSE, "PC: Altname als UTF-8 geschrieben");
    vcs_names_learn(L_GROESSE);
    from(U_GROESSE, back);
    SAME(back, L_GROESSE, "PC: Platte -> Altname im Repository");
    /* a local name that is not UTF-8 (Linux): kept as it is, found again */
    vcs_names_reset();
    from(L_GROESSE, back);
    SAME(back, L_GROESSE, "PC: Latin-1 auf der Platte bleibt");
    CHECK(vcs_local_join(out, sizeof(out), "/r", back) && strcmp(out, "/r/" L_GROESSE) == 0,
          "PC: und wird wieder gefunden");
    /* decomposed name on disk: NFC in the repository, NFD found again */
    vcs_names_reset();
    from(NFD_GROESSE, back);
    SAME(back, U_GROESSE, "PC: NFD -> NFC");
    CHECK(vcs_local_join(out, sizeof(out), "/r", U_GROESSE) && strcmp(out, "/r/" NFD_GROESSE) == 0,
          "PC: NFD-Schreibweise der Platte");
    /* an older NFD name in the repository */
    vcs_names_reset();
    vcs_names_learn(NFD_GROESSE);
    from(U_GROESSE, back);
    SAME(back, NFD_GROESSE, "PC: alter NFD-Name bleibt");
    from(NFD_GROESSE, back);
    SAME(back, NFD_GROESSE, "PC: alter NFD-Name, NFD auf der Platte");

    {
        char abs[512];
        cs_test_mode(CS_TEST_LATIN1);
        vcs_names_reset();
        CHECK(vcs_local_join(abs, sizeof(abs), "Work:repo", U_ORDNER "/" U_GROESSE)
              && strstr(abs, L_ORDNER "/" L_GROESSE) != NULL, "local_join wandelt um");
        CHECK(!vcs_local_join(abs, 8, "Work:repo", U_ORDNER) && abs[0] == '\0', "zu lang: leer");
    }

    printf("vcsname: %d ok, %d Fehler\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
