/*
 * PORT: port-written for angband-pico, stage 100.
 *
 * Host-side unit checks for src/platform/battery.h, run by tools/host-build.sh beside
 * utf8-test. The host build has no PICOCALC and cannot show the battery field, and the
 * device cannot reach 15 % or 5 % in a session, so this is where the decode, the colour
 * level, the field width and the warning state machine are proved.
 *
 *   gcc -std=gnu99 -Isrc/platform -o battery-test tools/battery-test.c
 */
#include <stdio.h>
#include <string.h>
#include "battery.h"

/* battery.c's two globals. battery.c itself needs the SDK and is not linked here. */
uint8_t battery_raw = 0;
bool battery_dirty = false;

static int fails;

static void ck(int ok, const char *what)
{
	if (!ok) { printf("FAIL %s\n", what); fails++; }
	else printf("ok %s\n", what);
}

static void ck_decode(uint8_t v, bool want_ok, int want_pct, bool want_chg, const char *what)
{
	int pct = -1;
	bool chg = false;
	bool ok = battery_decode(v, &pct, &chg);

	if (!want_ok)
		ck(!ok, what);
	else
		ck(ok && pct == want_pct && chg == want_chg, what);
}

static void ck_warn(struct battery_warn *w, int pct, bool chg, int want, const char *what)
{
	int got = battery_warn_step(w, pct, chg);

	if (got != want)
		printf("  (%s: got %d, wanted %d)\n", what, got, want);
	ck(got == want, what);
}

int main(void)
{
	struct battery_warn w;
	wchar_t field_w[BATTERY_FIELD_COLS];
	int pct;
	bool chg;

	/* The register byte. */
	ck_decode(0x00, false, 0, false, "decode 0x00 is no reading");
	ck_decode(87, true, 87, false, "decode 87 is 87 % on battery");
	ck_decode(0x80 | 87, true, 87, true, "decode 0x80|87 is 87 % charging");
	ck_decode(0x80, true, 0, true, "decode 0x80 is 0 % charging, a reading");
	ck_decode(100, true, 100, false, "decode 100 is 100 %");
	ck_decode(0x7F, true, 100, false, "decode 127 clamps to 100");
	ck_decode(0xFF, true, 100, true, "decode 0xFF clamps to 100 charging");
	ck_decode(1, true, 1, false, "decode 1 is 1 %");

	/* battery_read() and battery_changed() over the globals. */
	battery_raw = 0;
	ck(!battery_read(&pct, &chg), "read with nothing read yet is false");
	battery_raw = 0x80 | 42;
	battery_dirty = true;
	ck(battery_read(&pct, &chg) && pct == 42 && chg, "read returns the stored byte");
	ck(battery_changed(), "changed is true once after a change");
	ck(!battery_changed(), "changed is false the second time");

	/* The colour level: L_BLUE charging, L_GREEN >= 50, YELLOW 20-49, L_RED < 20. */
	ck(battery_level(100, false) == BATTERY_LEVEL_GOOD, "level 100 good");
	ck(battery_level(50, false) == BATTERY_LEVEL_GOOD, "level 50 good");
	ck(battery_level(49, false) == BATTERY_LEVEL_LOW, "level 49 low");
	ck(battery_level(20, false) == BATTERY_LEVEL_LOW, "level 20 low");
	ck(battery_level(19, false) == BATTERY_LEVEL_CRITICAL, "level 19 critical");
	ck(battery_level(1, false) == BATTERY_LEVEL_CRITICAL, "level 1 critical");
	ck(battery_level(5, true) == BATTERY_LEVEL_CHARGING, "level 5 charging is charging");
	ck(battery_level(100, true) == BATTERY_LEVEL_CHARGING, "level 100 charging is charging");

	/*
	 * battery_field(), the builder ui-display.c draws from, against printf's "%s%3d%%" at every
	 * charge and both states -- the format run 1 drew with before it was taken off the flash
	 * text path.
	 */
	{
		int bad = 0;
		for (int p = 0; p <= 100; p++) {
			for (int c = 0; c <= 1; c++) {
				wchar_t w[BATTERY_FIELD_COLS];
				char want[BATTERY_FIELD_COLS + 8];
				int i;

				snprintf(want, sizeof(want), "%s%3d%%", c ? "Chg" : "Bat", p);
				battery_field(w, p, c);
				if (strlen(want) != BATTERY_FIELD_COLS) bad++;
				for (i = 0; i < BATTERY_FIELD_COLS; i++)
					if (w[i] != (wchar_t)(unsigned char)want[i]) bad++;
			}
		}
		ck(bad == 0, "field matches \"%s%3d%%\" for 0..100, both states");
		battery_field(field_w, 87, false);
		ck(field_w[0] == L'B' && field_w[3] == L' ' && field_w[4] == L'8' && field_w[5] == L'7'
			&& field_w[6] == L'%', "field reads \"Bat 87%\"");
		battery_field(field_w, 100, true);
		ck(field_w[0] == L'C' && field_w[3] == L'1' && field_w[4] == L'0' && field_w[5] == L'0',
			"field reads \"Chg100%\"");
		battery_field(field_w, 5, false);
		ck(field_w[3] == L' ' && field_w[4] == L' ' && field_w[5] == L'5', "field reads \"Bat  5%\"");
	}

	/* The warnings: one per crossing, re-armed above 20 % or on charging. */
	memset(&w, 0, sizeof(w));
	ck_warn(&w, 80, false, 0, "warn 80 nothing");
	ck_warn(&w, 16, false, 0, "warn 16 nothing");
	ck_warn(&w, 15, false, BATTERY_WARN_LOW, "warn 15 low");
	ck_warn(&w, 15, false, 0, "warn 15 again nothing");
	ck_warn(&w, 10, false, 0, "warn 10 nothing");
	ck_warn(&w, 18, false, 0, "warn 18 does not re-arm");
	ck_warn(&w, 14, false, 0, "warn 14 after 18 nothing");
	ck_warn(&w, 5, false, BATTERY_WARN_CRITICAL, "warn 5 critical");
	ck_warn(&w, 4, false, 0, "warn 4 nothing");
	ck_warn(&w, 3, false, 0, "warn 3 nothing");
	ck_warn(&w, 21, false, 0, "warn 21 re-arms");
	ck_warn(&w, 15, false, BATTERY_WARN_LOW, "warn 15 low after re-arm");
	ck_warn(&w, 3, true, 0, "warn 3 charging nothing, re-arms");
	ck_warn(&w, 3, false, BATTERY_WARN_CRITICAL, "warn 3 unplugged critical, low skipped");

	memset(&w, 0, sizeof(w));
	ck_warn(&w, 4, false, BATTERY_WARN_CRITICAL, "warn first reading 4 critical");
	ck_warn(&w, 14, false, 0, "warn 14 after critical nothing");

	memset(&w, 0, sizeof(w));
	ck_warn(&w, 14, false, BATTERY_WARN_LOW, "warn first reading 14 low");
	ck_warn(&w, 20, false, 0, "warn 20 does not re-arm");
	ck_warn(&w, 15, false, 0, "warn 15 after 20 nothing");

	if (fails) {
		printf("%d checks FAILED\n", fails);
		return 1;
	}
	return 0;
}
