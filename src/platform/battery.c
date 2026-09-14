// PORT: port-written for angband-pico, stage 100. See battery.h.
//
// THE COST OF ONE READ. southbridge.h runs i2c1 at SB_BAUDRATE, 10,000 Hz. A read is a
// one-byte write (the register number) and a two-byte read, each behind an address byte:
// four bytes of 9 bits plus two start and two stop conditions, about 40 clock periods, so
// roughly 4 ms of wire. That is why it is not on any path inside a turn: main-pico.c calls
// battery_poll() only on the waiting branch of check_events(), which is time the player
// spends between commands and which the turn instrument already subtracts as idle.
//
// BATTERY_FAKE (stage 100 item 9) replaces the register with a constant so the 15 % and 5 %
// warnings and the automatic save can be seen in a session. It is a console-build override
// only: CMakeLists.txt refuses it without ANGBAND_CONSOLE, and so does the #error below.

#if defined(BATTERY_FAKE) && !defined(ANGBAND_CONSOLE)
#error "BATTERY_FAKE needs ANGBAND_CONSOLE: configure with -DANGBAND_CONSOLE=ON"
#endif

#include "pico/stdlib.h"

#include "battery.h"
#include "southbridge.h"

uint8_t battery_raw = 0;
bool battery_dirty = false;

static bool battery_polled = false;
static uint64_t battery_last_us = 0;

void battery_poll(void)
{
	uint64_t now = time_us_64();
	uint8_t v;

	if (battery_polled && now - battery_last_us < (uint64_t)BATTERY_POLL_SECONDS * 1000000u)
		return;

	battery_polled = true;
	battery_last_us = now;

#ifdef BATTERY_FAKE
	v = (uint8_t)(BATTERY_FAKE);
#else
	v = sb_read_battery();
#endif

	if (v != battery_raw) {
		battery_raw = v;
		battery_dirty = true;
	}
}
