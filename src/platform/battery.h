// PORT: port-written for angband-pico, stage 100. The battery on the status row.
//
// WHAT THIS IS
//
// The PicoCalc is a battery unit and the game has no crash save, so a player who cannot see
// the charge loses everything since the last Ctrl-S. The keyboard MCU already measures it:
// south-bridge register 0x0B holds the AXP2101's percentage in bits 0-6 and the charging
// state in bit 7, refreshed by the MCU every 20 s (PicoCalc/Code/picocalc_keyboard/
// picocalc_keyboard.ino, sync_bat() and check_pmu_int()). southbridge.c's sb_read_battery()
// returns that byte.
//
// WHY THE HEADER HOLDS THE LOGIC
//
// A core source includes this file (src/game/ui-display.c, under PICOCALC), and
// tools/compile-sweep.sh compiles src/game/ with no SDK include path -- the turnlog.h rule.
// So nothing from the SDK is here: <stdbool.h>, <stdint.h> and <wchar.h> only. The decode, the colour
// level and the warning state machine are static inline functions over plain values, which
// does two things. tools/battery-test.c checks them on the host with no stubs. And
// ui-display.c, which runs from SRAM, reads the last register byte out of .bss without a
// call into flash-resident code (specifications.md 6.4: keep a hot path off the QMI).
//
// The one function that touches the I2C bus, battery_poll(), is in battery.c and is called
// only from main-pico.c's check_events(), while the game is waiting for a key.

#ifndef INCLUDED_BATTERY_H
#define INCLUDED_BATTERY_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

/** Seconds between two register reads. The MCU refreshes the register every 20 s. */
#define BATTERY_POLL_SECONDS    30

/** The two warnings, and the charge above which both are armed again. */
#define BATTERY_WARN_LOW        15
#define BATTERY_WARN_CRITICAL   5
#define BATTERY_REARM           20

/** Columns the status-row field takes: "Bat 87%", "Chg100%". */
#define BATTERY_FIELD_COLS      7

/** The last register byte, 0 until the first read. Written by battery.c only. */
extern uint8_t battery_raw;

/** True after battery_raw changed, until battery_changed() is called. */
extern bool battery_dirty;

/**
 * Read the register if BATTERY_POLL_SECONDS have passed since the last read, or if it has
 * never been read. Otherwise it returns at once. One I2C transaction when it reads.
 */
void battery_poll(void);

/**
 * Decode a register byte. Returns false for 0, which is what the register holds with no
 * battery fitted, on a keyboard firmware without the register, and after a failed read.
 */
static inline bool battery_decode(uint8_t v, int *percent, bool *charging)
{
	int p = v & 0x7F;

	if (v == 0)
		return false;
	if (p > 100)
		p = 100;
	*percent = p;
	*charging = (v & 0x80) != 0;
	return true;
}

/** The last reading. False when there is none to show. */
static inline bool battery_read(int *percent, bool *charging)
{
	return battery_decode(battery_raw, percent, charging);
}

/** True once per change of the register byte. */
static inline bool battery_changed(void)
{
	bool was = battery_dirty;

	battery_dirty = false;
	return was;
}

/**
 * The field's BATTERY_FIELD_COLS wide characters, not terminated: "Bat 87%", "Chg100%",
 * "Bat  5%". Built by hand rather than formatted, so ui-display.c can put it straight into
 * the term with Term_addch() and never reach the flash-resident text conversion that
 * Term_putstr() uses. `percent` is 0..100, as battery_decode() gives it.
 */
static inline void battery_field(wchar_t *out, int percent, bool charging)
{
	out[0] = charging ? L'C' : L'B';
	out[1] = charging ? L'h' : L'a';
	out[2] = charging ? L'g' : L't';
	out[3] = (percent >= 100) ? L'1' : L' ';
	out[4] = (percent >= 10) ? (wchar_t)(L'0' + (percent / 10) % 10) : L' ';
	out[5] = (wchar_t)(L'0' + percent % 10);
	out[6] = L'%';
}

/** How the field is coloured. ui-display.c maps these onto COLOUR_*. */
enum {
	BATTERY_LEVEL_CHARGING = 0,	/* L_BLUE, whatever the charge */
	BATTERY_LEVEL_GOOD,		/* L_GREEN, 50 % and above */
	BATTERY_LEVEL_LOW,		/* YELLOW, 20-49 % */
	BATTERY_LEVEL_CRITICAL		/* L_RED, below 20 % */
};

static inline int battery_level(int percent, bool charging)
{
	if (charging)
		return BATTERY_LEVEL_CHARGING;
	if (percent >= 50)
		return BATTERY_LEVEL_GOOD;
	if (percent >= 20)
		return BATTERY_LEVEL_LOW;
	return BATTERY_LEVEL_CRITICAL;
}

/**
 * The warning state machine. `announced` is the lowest threshold already warned about
 * since the last re-arm, 0 for none. `saved` is set by the caller when it has saved the
 * game, and is never cleared: one automatic save per boot.
 */
struct battery_warn {
	int announced;
	bool saved;
};

/**
 * Feed one reading. Returns BATTERY_WARN_LOW or BATTERY_WARN_CRITICAL when that warning
 * is due now, else 0. At most one warning per crossing; charging, or a charge above
 * BATTERY_REARM, arms both again. A first reading already at or below 5 % gives the
 * critical warning and skips the low one.
 */
static inline int battery_warn_step(struct battery_warn *w, int percent, bool charging)
{
	if (charging || percent > BATTERY_REARM) {
		w->announced = 0;
		return 0;
	}
	if (percent <= BATTERY_WARN_CRITICAL && w->announced != BATTERY_WARN_CRITICAL) {
		w->announced = BATTERY_WARN_CRITICAL;
		return BATTERY_WARN_CRITICAL;
	}
	if (percent <= BATTERY_WARN_LOW && w->announced == 0) {
		w->announced = BATTERY_WARN_LOW;
		return BATTERY_WARN_LOW;
	}
	return 0;
}

#endif /* INCLUDED_BATTERY_H */
