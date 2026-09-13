/**
 * \file main-borg.c
 * \brief PORT: port-written for angband-pico, picocalc-device-harness stage 055.
 *
 * The PC half of lockstep. Angband's own borg plays a savefile HERE, inside a host build of
 * the port's own core, and this front end writes down two things:
 *
 *   <name>.keys   one harness key name per line (specifications.md 8.3), every keypress the
 *                 borg handed the game, in order.
 *   <name>.sync   one SYNC line per completed player command, from src/platform/sync.c --
 *                 the same formatter the device compiles.
 *
 * The harness then sends the keys to the device over serial and compares the device's SYNC
 * lines against these. Two simulations given the same savefile and the same keys must reach
 * the same state; a mismatch is a divergence and the run stops at it.
 *
 * THE BORG IS NOT ON THE DEVICE and nothing in src/borg/ is compiled for it. The device
 * runs the stock game and reads bytes off a UART. That is the whole point of the design:
 * the borg's input is the entire live game state, so forwarding its hooks over the wire
 * would be serialising the game every turn. Forwarding its OUTPUT is 60 bytes a command.
 *
 * WHY THIS FILE HAS ITS OWN main() INSTEAD OF BEING A MODULE IN src/host/main.c
 *
 * main.c's modules[] would have to gain an entry, and main.c and main-test.c are vendored
 * upstream files that the suite's part 2 builds unmodified. A separate main() costs the
 * ~60 lines below -- init_stuff, savefile_set_name, init_angband, textui_init, play_game --
 * and leaves both of them alone. tools/host-build.sh --borg links this file INSTEAD of
 * those two.
 *
 * THE THREE THINGS THAT MAKE THE KEYSTREAM USABLE BY THE DEVICE
 *
 *   1. The term is 64x32, not upstream's 80x24. The device's panel is 64x32 and the borg
 *      reads the screen; a different width is a different game. "-s WxH" sets it.
 *   2. Every key is checked against specifications.md 8.3 as it is recorded. A keycode the
 *      harness has no byte for -- an arrow, a modifier, anything >= 0x80 that is not one of
 *      the four named codes -- STOPS THE RUN at that key, because a keystream the device
 *      cannot replay exactly is worth nothing after the first gap.
 *   3. --replay reads a keystream back and feeds it to a fresh game through the SAME
 *      inkey_hack the borg used. If the replay's SYNC file matches the borg's, the capture
 *      is complete and the game is deterministic under this savefile -- both proved on the
 *      PC, before a four-minute device boot is spent on it.
 */

#include "angband.h"
#include "borg/borg.h"
#include "buildid.h"
#include "cmd-core.h"
#include "cmds.h"
#include "game-world.h"
#include "init.h"
#include "main.h"
#include "player.h"
#include "savefile.h"
#include "sync.h"
#include "z-rand.h"
#include "ui-command.h"
#include "ui-display.h"
#include "ui-game.h"
#include "ui-input.h"
#include "ui-init.h"
#include "ui-output.h"
#include "ui-prefs.h"
#include "ui-term.h"

#include <errno.h>
#include <time.h>

/*
 * THE SYNC LINE, OUT OF THE DEVICE'S OWN FORMATTER.
 *
 * src/platform/sync-fmt.h is included by exactly two sources in this workspace: this file
 * and src/platform/main-pico.c. Everything in it is static, so each side gets its own copy
 * and nothing is exported; what matters is that the BYTES ARE PRODUCED BY THE SAME CODE.
 * Two sides that could disagree about the format would make every comparison between them
 * worthless, and a format that is "the same" because someone kept two copies in step is one
 * merge away from not being.
 *
 * Only the last step differs. On the device sync_end() is a printf to the console; here it
 * is a write to the file this front end opened, and it does nothing at all until that file
 * is handed over -- which happens at the first key of the keystream and not before, so
 * record 1 is the command key 1 drove.
 */
#include "sync-fmt.h"

static FILE *sync_out;

void sync_end(int32_t game_turn)
{
	char line[160];
	int  len;

	if (!player || !sync_out) return;

	sync_n++;

	len = sync_format(line, sizeof(line), sync_n, game_turn);
	if (len > 0) {
		fputs(line, sync_out);
		fflush(sync_out);
	}
}

/** Where sync_end() writes from now on, and the record numbering restarts. */
static void sync_host_set_file(FILE *f)
{
	sync_out = f;
	sync_n = 0;
}

/** How many SYNC lines have been written, i.e. the last record number. */
static uint32_t sync_host_count(void)
{
	return sync_n;
}

/**
 * The `rs` field on its own, so this front end can print the RNG fingerprint at a moment
 * that is not the end of a command -- which is what it takes to find out whether something
 * OUTSIDE the game loop moved the generator. It is, and borg-init.c:488 is where; see
 * rng_save() below.
 */
static uint32_t sync_host_rng(void)
{
	return sync_rng_digest();
}

/* cmd-misc.c, under ALLOW_BORG. Declared there and nowhere public. */
extern void do_cmd_borg(void);

/* ---------------------------------------------------------------------------------------
 * Options, all from the command line.
 */
static const char *opt_savefile  = NULL;	/* -u<name> */
static const char *opt_out       = "borg";	/* -o<stem>: writes <stem>.keys/.sync/.log */
static const char *opt_replay    = NULL;	/* -r<file>: replay instead of thinking */
static long	   opt_turns     = 1000;	/* -t<n>: stop after n completed commands */
static int	   opt_wid       = 64;		/* -s WxH */
static int	   opt_hgt       = 32;
/*
 * THE SIDEBAR MODE IS IN THE SAVEFILE, SO THIS ONLY BITES ON -n.
 *
 * It is part of the geometry and not a cosmetic choice: ui-term.h derives ROW_MAP,
 * ROW_BOTTOM_MAP and COL_MAP from it, so two modes put the map in different places and give
 * it a different size, and the borg reads the screen. A mismatch would matter.
 *
 * It cannot happen, and it took a bench session to see why. save.c:323 writes
 * SIDEBAR_MODE and load.c:442 reads it back over whatever the front end set -- so
 * src/platform/main-pico.c:881 asking for SIDEBAR_TOP is overruled the moment the device
 * loads a savefile written at SIDEBAR_LEFT, which is exactly what the panel showed on
 * 2026-09-12. The two sides agree by construction as long as they share a savefile, which
 * lockstep requires anyway.
 *
 * So the default here is ui-term.c:3254's, the one a fresh term gets, and -b matters only
 * when -n is about to write a savefile.
 */
static int	   opt_sidebar   = SIDEBAR_LEFT;
static int	   opt_verbose   = 0;		/* -v */
static const char *opt_dir       = NULL;	/* -d<dir>: the lib/ tree to read */
static int	   opt_new       = 0;		/* -n: birth a character and save it */

/* ---------------------------------------------------------------------------------------
 * State.
 */
static FILE *keys_file;
static FILE *sync_file;

static long   keys_written;
static long   stop_reason_set;
static char   stop_reason[256];

/* The borg's own hook, captured so the recorder can forward to it. */
static struct keypress (*real_hack)(int flush_first);

static int  start_phase;	/* 0 nothing sent, 1 Ctrl-Z sent, 2 running */
static bool finished;
static bool saved;		/* -n only: the Ctrl-X has been sent */
static bool rng_noted_playing;
static bool rng_noted_source;

/* ---------------------------------------------------------------------------------------
 * THE ONE RNG DRAW THE DEVICE DOES NOT MAKE.
 *
 * borg-init.c:488 is
 *
 *     if (!borg_rand_local) borg_rand_local = randint1(0x10000000);
 *
 * inside borg_init(). It seeds the borg's own local generator from the GAME's generator,
 * once, when the borg is started -- and starting the borg is something only this side does.
 * The device has no ALLOW_BORG, no Ctrl-Z and no borg_init(); it loads the savefile and
 * waits for a key. So from the very first command the two sides would be one RNG call
 * apart, and Angband's first act on a `>` is to generate a dungeon level: the divergence
 * shows up as a DIFFERENT CAVE, which reads like a port bug and is not one.
 *
 * Measured 2026-09-12, and this is the whole of it: at the moment the game became playable
 * both sides read si=19 rs=853891f9; at the moment a key source was attached, the replay
 * still read si=19 rs=853891f9 and the borg run read si=18 rs=09765c7a. One call.
 *
 * So the state is copied out before the borg is started and written back once it is
 * running. This is NOT the rejected "re-seed from the device" of the stage plan's Optional
 * fixes -- that one resets the RNG every turn and hides real divergence. This is one
 * restore, before key 1, undoing a side effect of a PC-side tool that is not part of the
 * game; after it the generator is exactly where the savefile left it, which is exactly
 * where the device's is. The proof it is right is that --replay, which never starts a borg
 * and is therefore the device's situation, then reproduces the borg run byte for byte.
 *
 * borg_rand_local keeps the value it drew, so the borg stays as deterministic as it was;
 * its seed is a function of the savefile either way.
 */
static uint32_t rng_saved_state[RAND_DEG];
static uint32_t rng_saved_i;
static uint32_t rng_saved_value;
static bool	rng_saved_quick;
static bool	rng_saved;

static void rng_save(void)
{
	int i;

	for (i = 0; i < RAND_DEG; i++) rng_saved_state[i] = STATE[i];
	rng_saved_i = state_i;
	rng_saved_value = Rand_value;
	rng_saved_quick = Rand_quick;
	rng_saved = true;
}

static void rng_restore(void)
{
	int i;

	if (!rng_saved) return;
	for (i = 0; i < RAND_DEG; i++) STATE[i] = rng_saved_state[i];
	state_i = rng_saved_i;
	Rand_value = rng_saved_value;
	Rand_quick = rng_saved_quick;
}

/*
 * -n: birth a character, take one step, save and quit.
 *
 * This is character-for-character the key list in the harness's own
 * tools/harness/sessions/angband-birth.txt, which was written against the 64x32 panel in
 * angband-picocalc stage 060's screen capture: Human / Warrior / point-based, every choice
 * the first entry of its menu and therefore a bare Enter. It is here so the SAME key list
 * births the host's savefile and the device's -- a savefile difference is a divergence by
 * definition, and two birth routines are two chances to differ.
 *
 * Two Escapes first: the splash takes any key, and an Enter that arrives after the splash
 * has already gone opens Angband's command menu, which then eats every key after it
 * (harness stage 050).
 *
 * THE LIST STOPS AT THE CONFIRM, AND THE BORG IS STARTED BEFORE THE SAVE. borg_cmd_start()
 * calls borg_reinit_options(), which sets ELEVEN player options and clears the ignore
 * tables (borg-init.c:400). pickup_always alone changes what stepping on an item does, and
 * auto_more changes whether a -more- prompt exists at all -- and every one of those is
 * saved in the savefile and loaded by the device. If the borg reset them on the PC after
 * the device had loaded the file, the two games would be playing under different rules
 * from the first command. So -n births the character, starts the borg, lets it take
 * -t commands, and only then saves: the savefile the device loads already carries the
 * options the borg would have set, and the borg's own reinit on the next run is a no-op.
 */
static const char *birth_keys[] = {
	"esc", "esc",
	"enter",			/* race:   a) Human */
	"enter",			/* class:  a) Warrior */
	"enter",			/* roller: a) Point-based */
	"enter",			/* points: accept as rolled */
	"H", "A", "R", "N", "E", "S", "S",
	"enter",			/* name */
	"y",				/* accept character history */
	"enter",			/* confirm */
	NULL
};
static int birth_at;

/* Replay mode. */
static keycode_t *replay_code;
static long	  replay_n;
static long	  replay_at;

static clock_t run_t0;

/**
 * Print what happened and leave.
 *
 * quit() here runs no quit_aux hook -- this front end installs none -- so nothing is
 * written to the savefile on the way out. That is deliberate and is checked in two places:
 * a play or replay run must leave the savefile exactly as the device's card was staged
 * from, and a dead character must not reach close_game(), which deletes it.
 */
static void report_and_quit(void)
{
	double secs = (double)(clock() - run_t0) / CLOCKS_PER_SEC;

	printf("borg: stopped after %lu commands, %ld keys, %.1f s\n",
		(unsigned long)sync_host_count(), keys_written, secs);
	printf("borg: reason: %s\n", stop_reason_set ? stop_reason : "the game loop ended");
	if (player) {
		printf("borg: depth %d, clevel %d, chp %d, au %ld, dead %d\n",
			(int)player->depth, (int)player->lev, (int)player->chp,
			(long)player->au, (int)player->is_dead);
	}

	if (keys_file) { fclose(keys_file); keys_file = NULL; }
	if (sync_file) { fclose(sync_file); sync_file = NULL; }

	quit(NULL);
}

static void stop_run(const char *fmt, ...)
{
	va_list vp;

	if (stop_reason_set) return;
	va_start(vp, fmt);
	vstrnfmt(stop_reason, sizeof(stop_reason), fmt, vp);
	va_end(vp);
	stop_reason_set = 1;
	finished = true;
}

/* ---------------------------------------------------------------------------------------
 * specifications.md 8.3, from the other end.
 *
 * The driver turns a name into a byte and the device turns that byte into a keycode; this
 * turns a keycode back into the name, and the three tables have to agree exactly. The
 * awkward rows are the ones the table calls out:
 *
 *   0x20   is written "space", because a script splits its arguments on whitespace.
 *   0x7F   is written "del": not printable, so a script cannot write the byte.
 *   KTRL   0x01..0x1A is written "ctrl-a".."ctrl-z", except that 0x08, 0x09 and 0x0D are
 *          claimed by earlier rows -- they come back as backspace, tab and enter, which is
 *          what the device will deliver for them, and is why "ctrl-h", "ctrl-i" and
 *          "ctrl-m" are not what they look like.
 *   0x0A   HAS NO NAME. ctrl-j is refused by the driver because the device drops the byte,
 *          so a borg that produced KTRL('J') would produce a key the harness cannot send.
 *
 * Returns false for anything with no name, which stops the run.
 */
static bool key_name(struct keypress k, char *buf, size_t cap)
{
	keycode_t c = k.code;

	if (k.mods & (KC_MOD_CONTROL | KC_MOD_SHIFT | KC_MOD_ALT | KC_MOD_META |
			KC_MOD_KEYPAD)) {
		/*
		 * The device sets one of KTRL(x) or KC_MOD_CONTROL and never both
		 * (specifications.md 6.4), and it has no byte at all for shift, alt, meta or
		 * keypad. A modifier here is unrepresentable whatever the code is.
		 */
		strnfmt(buf, cap, "MODS+0x%02x/0x%lx", (unsigned)k.mods, (unsigned long)c);
		return false;
	}

	if (c == KC_ENTER)	{ my_strcpy(buf, "enter", cap);		return true; }
	if (c == KC_TAB)	{ my_strcpy(buf, "tab", cap);		return true; }
	if (c == KC_BACKSPACE)	{ my_strcpy(buf, "backspace", cap);	return true; }
	if (c == ESCAPE)	{ my_strcpy(buf, "esc", cap);		return true; }
	if (c == 0x7F)		{ my_strcpy(buf, "del", cap);		return true; }
	if (c == 0x20)		{ my_strcpy(buf, "space", cap);		return true; }

	if (c > 0x20 && c <= 0x7E) {
		buf[0] = (char)c;
		buf[1] = '\0';
		return true;
	}

	/* 0x0A has no byte the driver will send; every other control in the range does. */
	if (c >= 0x01 && c <= 0x1A && c != 0x0A) {
		strnfmt(buf, cap, "ctrl-%c", (char)('a' + (int)c - 1));
		return true;
	}

	strnfmt(buf, cap, "0x%lx", (unsigned long)c);
	return false;
}

/** The inverse, for --replay. Returns false on a name this build cannot decode. */
static bool key_code(const char *name, keycode_t *out)
{
	if (streq(name, "enter") || streq(name, "return"))	{ *out = KC_ENTER;	return true; }
	if (streq(name, "tab"))					{ *out = KC_TAB;	return true; }
	if (streq(name, "backspace") || streq(name, "bs"))	{ *out = KC_BACKSPACE;	return true; }
	if (streq(name, "esc") || streq(name, "escape"))	{ *out = ESCAPE;	return true; }
	if (streq(name, "del") || streq(name, "delete"))	{ *out = 0x7F;		return true; }
	if (streq(name, "space"))				{ *out = 0x20;		return true; }

	if (strlen(name) == 1 && name[0] > 0x20 && name[0] <= 0x7E) {
		*out = (keycode_t)(unsigned char)name[0];
		return true;
	}

	if (strlen(name) == 6 && prefix(name, "ctrl-") &&
			name[5] >= 'a' && name[5] <= 'z') {
		*out = (keycode_t)(name[5] - 'a' + 1);
		return true;
	}

	return false;
}

/* ---------------------------------------------------------------------------------------
 * The recorder.
 *
 * inkey_ex() calls (*inkey_hack)() and returns whatever comes back, bypassing keymaps,
 * Term_inkey and inkey_aux entirely. So the keycodes the borg produces are exactly the
 * keycodes the game consumes, and on the device the keycodes push_serial_key() produces are
 * exactly the keycodes the game consumes. The two paths meet at the same place, which is
 * what makes a keystream transferable at all -- and it is only true because this tree ships
 * no keymaps: lib/customize/*.prf has not one A: line, so keymap_find() never fires.
 *
 * A returned EVT_NONE is not a key: inkey_ex() falls through to the real terminal for it,
 * and it must not be written down.
 */
/**
 * Start the SYNC file at the FIRST KEY OF THE KEYSTREAM, not before.
 *
 * sync_host_set_file() restarts the record numbering, so calling it here makes record 1 the
 * command that key 1 drove. Doing it any earlier costs a spurious leading record: arming
 * the recorder happens inside the play_game() iteration that processed Ctrl-Z, and that
 * iteration completes with an empty command queue -- one SYNC line describing a state
 * nothing changed. Measured 2026-09-12: the borg run wrote 1,001 records where the replay
 * of its own 1,166 keys wrote 1,000, and the extra one was that.
 *
 * The device has the same hazard from the other end: any key a session script sends before
 * the keystream that happens to complete a command adds a record the host does not have.
 * That is survivable -- `d` covers the fields and not the number (see sync.c) -- but it is
 * one more thing for a divergence report to have to explain, so both sides start where the
 * keystream starts wherever that is arrangeable.
 */
static void attach_sync_file(void)
{
	static bool done;

	if (done) return;
	done = true;
	sync_host_set_file(sync_file);
}

static void record_key(struct keypress k)
{
	char name[64];

	if (k.type != EVT_KBRD) return;

	if (!key_name(k, name, sizeof(name))) {
		stop_run("key %ld is %s, which specifications.md 8.3 has no byte for",
			keys_written + 1, name);
		return;
	}

	attach_sync_file();

	if (keys_file) {
		fprintf(keys_file, "%s\n", name);
		fflush(keys_file);
	}
	keys_written++;
}

static struct keypress record_hack(int flush_first)
{
	struct keypress k = (*real_hack)(flush_first);

	record_key(k);
	return k;
}

/**
 * Take over whatever hook the borg has just installed.
 *
 * borg_update_entrypoint() is called from borg_cmd_start(), borg_cmd_step() and
 * borg_cmd_update(), so the hook can be reinstalled behind this front end's back; every
 * term hook calls this, which is the cheapest way to be sure the recorder is the one the
 * game sees. real_hack is captured once, before the borg is ever started (see main()), so
 * this is only ever a swap of an already-known pointer.
 */
static void check_caps(void);
static struct keypress replay_hack(int flush_first);

static void arm_recorder(void)
{
	check_caps();

	/*
	 * The RNG fingerprint at the two moments that are NOT the end of a command: the first
	 * time the game is playing, and the first time a key source is attached. If the two
	 * runs agree on the first and disagree on the second, whatever happens between them
	 * -- borg_init() on the play side, nothing at all on the device -- is moving the
	 * generator, and lockstep cannot hold until it is dealt with.
	 */
	if (!rng_noted_playing && player && player->upkeep && player->upkeep->playing) {
		rng_noted_playing = true;
		rng_save();
		printf("rng: playing        si=%lu rs=%08lx (saved)\n",
			(unsigned long)state_i, (unsigned long)sync_host_rng());
	}

	if (opt_replay) {
		/*
		 * WHERE A REPLAY IS ALLOWED TO START, AND WHY IT IS NOT main().
		 *
		 * The borg run did not begin at the splash. It began at the first
		 * cmd_get_hook() of the first play_game() iteration in which
		 * player->upkeep->playing was true -- everything before that was the load
		 * screen, answered by this front end with Escapes that were never recorded.
		 * A replay that starts feeding the keystream any earlier spends its first
		 * keys on those prompts and is a different game from the second key onward.
		 *
		 * THIS IS ALSO THE DEVICE'S RULE. The device has no borg and no Ctrl-Z: the
		 * harness dismisses the splash and then sends key 1. So the point the hook
		 * goes in here is the point the session script must reach before it starts
		 * the keystream, and a divergence at key 1 means the two did not agree on it.
		 *
		 * It is installed from a term hook rather than from inside the key request,
		 * because inkey_ex() consults inkey_hack BEFORE entering its Term_inkey
		 * loop: a hook installed during the loop is not seen until the next call,
		 * and the loop would spin with nothing feeding it. Every term hook runs
		 * under Term_fresh(), and pre_turn_refresh() calls Term_fresh() at the top
		 * of each play_game() iteration -- outside the key request, which is what is
		 * needed.
		 */
		if (!inkey_hack && player && player->upkeep && player->upkeep->playing) {
			inkey_hack = replay_hack;
			printf("rng: key source    si=%lu rs=%08lx (no borg)\n",
				(unsigned long)state_i, (unsigned long)sync_host_rng());
			printf("borg: replay hook installed, %ld keys to feed\n", replay_n);
		}
		return;
	}

	if (inkey_hack && inkey_hack != record_hack) {
		inkey_hack = record_hack;
		if (!rng_noted_source) {
			rng_noted_source = true;
			printf("rng: key source    si=%lu rs=%08lx (borg started)\n",
				(unsigned long)state_i, (unsigned long)sync_host_rng());
			rng_restore();
			printf("rng: restored      si=%lu rs=%08lx\n",
				(unsigned long)state_i, (unsigned long)sync_host_rng());
		}
	}
}

/* ---------------------------------------------------------------------------------------
 * Replay mode.
 *
 * The same hook, fed from a file. This is not a simulation of the device -- it is the same
 * keycodes arriving in the same order at the same place -- but it is the cheap half of the
 * device's job, and it answers the two questions that would otherwise cost a bench session:
 * did the recorder miss a key, and is this game deterministic under this savefile.
 */
static struct keypress replay_hack(int flush_first)
{
	struct keypress k = { EVT_KBRD, 0, 0 };

	(void)flush_first;

	/*
	 * A POLL IS NOT A KEY REQUEST, AND ANSWERING ONE CANCELS A REST.
	 *
	 * ui-game.c:673, check_for_player_interrupt(), sets inkey_scan = SCAN_INSTANT and asks
	 * inkey_ex() whether anything is waiting -- every 128 game turns while the player is
	 * resting, and on every step of a run or a repeat. It is asking "has the human hit a
	 * key to stop this", and a hook that always answers says yes: the rest is disturbed,
	 * "Cancelled." is printed, and the key is eaten. Measured 2026-09-12: a replay that
	 * answered these matched the borg run for 516 commands and then took a step where the
	 * borg had rested 220 game turns back to full hit points.
	 *
	 * The device cannot answer them either, and that is the point. The harness sends one
	 * key per COMPLETED command and then waits for the SYNC line, so while a command is
	 * running the device's term queue is empty and every one of these polls comes back
	 * empty. Returning EVT_NONE here is not a convenience, it is the model of the wire.
	 */
	if (inkey_scan) {
		k.type = EVT_NONE;
		return k;
	}

	if (replay_at >= replay_n) {
		stop_run("keystream exhausted after %ld keys", replay_n);
		k.type = EVT_NONE;
		return k;
	}

	attach_sync_file();

	k.code = replay_code[replay_at++];
	return k;
}

static bool replay_load(const char *path)
{
	char  line[128];
	FILE *f = fopen(path, "r");
	long  cap = 256;

	if (!f) {
		printf("borg: cannot open keystream %s: %s\n", path, strerror(errno));
		return false;
	}

	replay_code = mem_zalloc(sizeof(keycode_t) * cap);

	while (fgets(line, sizeof(line), f)) {
		keycode_t code;
		char	 *s = line;
		size_t	  n;

		n = strlen(s);
		while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
		if (n == 0) continue;

		if (!key_code(s, &code)) {
			printf("borg: keystream line %ld: cannot decode '%s'\n",
				replay_n + 1, s);
			fclose(f);
			return false;
		}

		if (replay_n == cap) {
			cap *= 2;
			replay_code = mem_realloc(replay_code, sizeof(keycode_t) * cap);
		}
		replay_code[replay_n++] = code;
	}

	fclose(f);
	printf("borg: replay loaded %ld keys from %s\n", replay_n, path);
	return true;
}

/* ---------------------------------------------------------------------------------------
 * The term. Written from main-test.c next door, with the hooks doing nothing: ui-term.c
 * maintains Term->scr whatever the front end does with the cells, and Term->scr is what the
 * borg reads. What the hooks DO do is call arm_recorder(), because every one of them is
 * reached from inside the borg's own key routine.
 */
typedef struct { term t; } term_data;
static term_data td;

static void term_init_borg(term *t)  { (void)t; if (opt_verbose) printf("term-init %s\n", buildid); }
static void term_nuke_borg(term *t)  { (void)t; }
static errr term_curs_borg(int x, int y) { (void)x; (void)y; arm_recorder(); return 0; }
static errr term_wipe_borg(int x, int y, int n) { (void)x; (void)y; (void)n; arm_recorder(); return 0; }

static errr term_text_borg(int x, int y, int n, int a, const wchar_t *s)
{
	(void)x; (void)y; (void)n; (void)a; (void)s;
	arm_recorder();
	return 0;
}

/**
 * The one hook with a decision in it.
 *
 * Term_xtra(TERM_XTRA_EVENT, v) arrives with v == 0 from the borg's own "has the user
 * pressed anything?" poll near the end of internal_borg_inkey(), and with v != 0 when the
 * game is genuinely blocked waiting for a key. PUSHING A KEY ON THE v == 0 CALL WOULD READ
 * AS A USER ABORT and the borg would shut itself down; so the poll only re-arms, and only
 * the blocking call is allowed to produce anything.
 *
 * The blocking call happens three times in a normal run: twice to start the borg, and once
 * at the end when the borg has stopped feeding keys and the game wants one from a human.
 */
/**
 * Print the top two rows of the term, which is where every prompt this front end has to
 * answer appears. -v only: it is a debugging aid for the key lists above, not a record --
 * the device's own answer to the same question is harness.py screen (specifications.md 8.5).
 */
static void dump_top(const char *tag)
{
	char line[256];
	int  w, h, x, y;

	Term_get_size(&w, &h);
	if (w > (int)sizeof(line) - 1) w = (int)sizeof(line) - 1;

	for (y = 0; y < 2 && y < h; y++) {
		for (x = 0; x < w; x++) {
			int	a = 0;
			wchar_t c = 0;

			if (Term_what(x, y, &a, &c) != 0) c = L' ';
			line[x] = (c >= 32 && c < 127) ? (char)c : ' ';
		}
		line[w] = '\0';
		printf("%s row%d |%s|\n", tag, y, line);
	}
}

static errr term_xtra_event_borg(int v)
{
	arm_recorder();

	if (v == 0) return 0;

	if (opt_verbose) {
		printf("want-key: phase=%d birth=%d playing=%d finished=%d\n",
			start_phase, birth_at,
			(player && player->upkeep) ? (int)player->upkeep->playing : -1,
			(int)finished);
		dump_top("want-key:");
	}

	/* The game wants a key and nothing is supplying one. */
	if (finished) {
		if (opt_new && !saved) {
			/*
			 * Ctrl-X is "Save and quit" (ui-game.c:217). Letting the GAME write
			 * the savefile, rather than calling save_game() from here, is the
			 * point: the device loads what Angband's own save path produced.
			 */
			saved = true;
			printf("borg: birth done, saving with Ctrl-X\n");
			Term_keypress(KTRL('X'), 0);
			return 0;
		}
		report_and_quit();
		return 0;
	}

	if (opt_new && birth_keys[birth_at]) {
		keycode_t code;


		if (!key_code(birth_keys[birth_at], &code)) {
			stop_run("birth: cannot decode '%s'", birth_keys[birth_at]);
			quit(NULL);
			return 0;
		}
		if (opt_verbose) printf("birth: key %d '%s'\n", birth_at, birth_keys[birth_at]);
		birth_at++;
		Term_keypress(code, 0);
		return 0;
	}

	if (!player || !player->upkeep->playing) {
		/* A prompt before the game proper. Escape leaves every one of them. */
		Term_keypress(ESCAPE, 0);
		return 0;
	}

	if (opt_replay) {
		/*
		 * arm_recorder() installs the hook the moment the game is playing, and the
		 * hook only returns EVT_NONE when the keystream has run out -- so reaching
		 * the terminal means the stream is exhausted, or that this replay needed MORE
		 * keys than the borg run produced, which is the interesting case and is why
		 * the count is printed.
		 */
		stop_run("replay: the game asked the terminal for a key at %ld of %ld",
			replay_at, replay_n);
		report_and_quit();
		return 0;
	}

	switch (start_phase) {
	case 0:
		/*
		 * NOSCORE_BORG is what do_cmd_try_borg() sets after asking "are you sure".
		 * Setting it first skips two messages, a message flush and a get_check(),
		 * which between them would need three more keys and could meet a -more-.
		 * It is saved in the savefile and read back by load.c, so this IS a state
		 * difference between this run and the device's -- and it is the only one.
		 * It is also inert: the flag is read by score.c alone (generate.c looks at
		 * NOSCORE_JUMPING, not this), so no decision in the simulation depends on
		 * it and no SYNC field can see it.
		 */
		player->noscore |= NOSCORE_BORG;
		Term_keypress(KTRL('Z'), 0);
		start_phase = 1;
		return 0;

	case 1:
		/* do_cmd_borg()'s get_com("Borg command (? for help): "). */
		Term_keypress('z', 0);
		start_phase = 2;
		return 0;

	default:
		/*
		 * The borg has stopped feeding keys and has not been asked to. Its own
		 * reasons -- borg_oops(), a stop level reached, the user-abort path -- all
		 * end here, and the log file says which.
		 */
		stop_run("the borg stopped feeding keys after %ld keys (borg_active=%d)",
			keys_written, (int)borg_active);
		report_and_quit();
		return 0;
	}
}

static errr term_xtra_borg(int n, int v)
{
	arm_recorder();

	if (n == TERM_XTRA_EVENT) return term_xtra_event_borg(v);

	return 0;
}

static void term_data_link_borg(void)
{
	term *t = &td.t;

	term_init(t, opt_wid, opt_hgt, 256);
	t->sidebar_mode = opt_sidebar;

	t->init_hook = term_init_borg;
	t->nuke_hook = term_nuke_borg;
	t->xtra_hook = term_xtra_borg;
	t->curs_hook = term_curs_borg;
	t->wipe_hook = term_wipe_borg;
	t->text_hook = term_text_borg;

	t->data = &td;

	Term_activate(t);
	angband_term[0] = t;
}

/* ---------------------------------------------------------------------------------------
 * The turn cap.
 *
 * sync.c counts completed player commands, which is the unit the device counts too, so the
 * cap is expressed in those and not in keys or in game turns. arm_recorder() calls this,
 * and every term hook calls arm_recorder(), so it runs far more often than once a command
 * -- which is fine: it is three compares against counters and it cannot be missed.
 *
 * Stopping means "stop feeding keys". borg_active = false makes internal_borg_inkey()
 * remove its own hook on its next call and return ESCAPE, which leaves whatever screen the
 * borg was on; the next blocking key request then lands in term_xtra_event_borg() above
 * and quits.
 */
static void check_caps(void)
{
	if (finished) return;

	if (player && player->is_dead) {
		/*
		 * Leave NOW, not by letting the loop end. play_game()'s loop exits on
		 * is_dead into close_game(true), and close_game() on a dead character
		 * DELETES THE SAVEFILE -- which on this machine is the master copy the
		 * device's card was staged from. quit() runs no quit_aux hook here, so
		 * nothing is written.
		 */
		stop_run("the character died at command %lu",
			(unsigned long)sync_host_count());
		borg_active = false;
		printf("borg: died at command %lu; leaving without saving\n",
			(unsigned long)sync_host_count());
		report_and_quit();
		return;
	}

	/*
	 * borg_respawning is set by the borg's death handler just before it reincarnates.
	 * A reincarnation is a NEW CHARACTER generated from the live RNG, which the device
	 * cannot follow by any means, so it ends the run rather than continuing it.
	 */
	if (borg_respawning > 0) {
		stop_run("the borg is reincarnating at command %lu; the device cannot follow",
			(unsigned long)sync_host_count());
		borg_active = false;
		return;
	}

	if ((long)sync_host_count() >= opt_turns) {
		stop_run("reached the %ld-command cap", opt_turns);
		borg_active = false;
		return;
	}
}

/* ---------------------------------------------------------------------------------------
 * main().
 */
static void usage(void)
{
	puts("Usage: angband-borg -u<savefile> [options]");
	puts("  -u<name>    savefile name (required)");
	puts("  -o<stem>    output stem; writes <stem>.keys and <stem>.sync (default borg)");
	puts("  -t<n>       stop after n completed player commands (default 1000)");
	puts("  -r<file>    replay a keystream instead of running the borg");
	puts("  -s<WxH>     term size (default 64x32, the PicoCalc panel)");
	puts("  -d<dir>     the lib/ tree to read (default the compiled-in path)");
	puts("  -n          birth a character into <savefile>, save and exit; do not play");
	puts("  -b<N>       sidebar mode: 0 left, 1 top (default, the device's), 2 none");
	puts("  -v          verbose");
	exit(1);
}

int main(int argc, char *argv[])
{
	char	path[1024];
	int	i;

	argv0 = argv[0];

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] != '-') usage();

		switch (a[1]) {
		case 'u': opt_savefile = a + 2; break;
		case 'o': opt_out = a + 2; break;
		case 'r': opt_replay = a + 2; break;
		case 't': opt_turns = atol(a + 2); break;
		case 'v': opt_verbose = 1; break;
		case 'd': opt_dir = a + 2; break;
		case 'n': opt_new = 1; break;
		case 'b':
			opt_sidebar = atoi(a + 2);
			if (opt_sidebar < 0 || opt_sidebar >= SIDEBAR_MAX) usage();
			break;
		case 's':
			if (sscanf(a + 2, "%dx%d", &opt_wid, &opt_hgt) != 2) usage();
			break;
		default: usage();
		}
	}

	if (!opt_savefile || !opt_savefile[0]) usage();

	/*
	 * -n wants the SHORTEST run that makes the borg initialise itself, not a game.
	 * One completed command is enough: borg_cmd_start() has already run by then.
	 */
	if (opt_new && opt_turns == 1000) opt_turns = 1;

	safe_setuid_drop();

	/*
	 * main.c's init_stuff(), inlined because it is static there and this file does not
	 * edit vendored sources. -d points all three paths at one tree, which is how the
	 * borg is given THE PORT'S OWN lib/ -- the same lib/gamedata the device reads off
	 * the SD card. A host borg reading a different monster.txt from the device is not a
	 * lockstep partner, it is a second game.
	 */
	{
		char configpath[512], libpath[512], datapath[512];

		my_strcpy(configpath, opt_dir ? opt_dir : DEFAULT_CONFIG_PATH, sizeof(configpath));
		my_strcpy(libpath,    opt_dir ? opt_dir : DEFAULT_LIB_PATH,    sizeof(libpath));
		my_strcpy(datapath,   opt_dir ? opt_dir : DEFAULT_DATA_PATH,   sizeof(datapath));

		if (!suffix(configpath, PATH_SEP)) my_strcat(configpath, PATH_SEP, sizeof(configpath));
		if (!suffix(libpath, PATH_SEP))    my_strcat(libpath, PATH_SEP, sizeof(libpath));
		if (!suffix(datapath, PATH_SEP))   my_strcat(datapath, PATH_SEP, sizeof(datapath));

		init_file_paths(configpath, libpath, datapath);

		/*
		 * THE DEVICE'S DIRECTORY LAYOUT, IMPOSED BY HAND.
		 *
		 * init_file_paths() has just put ANGBAND_DIR_USER at PRIVATE_USER_PATH, which
		 * config.h:68 defines as "~/.angband" on every UNIX build -- so the borg would
		 * read borg.txt and user.prf out of the WSL home directory while reading its
		 * gamedata out of -d. The device does not: it is a PICOCALC build, so UNIX is
		 * undefined, PRIVATE_USER_PATH does not exist, USE_PRIVATE_PATHS is set, and
		 * every writable directory hangs off <lib>/user/.
		 *
		 * Two builds of one game that read their pref files from different places are
		 * two different games, and the whole value of this front end is that it is not.
		 * So the five paths are rebuilt here to the device's shape. It is done after
		 * init_file_paths() rather than by a -D, because the alternatives all have side
		 * effects: -DPRIVATE_USER_PATH leaves the "Angband" subdirectory on the end,
		 * and -DDJGPP (the other way to switch config.h:68 off) also changes
		 * file_get_savefile() in z-file.c, which calls Rand_simple().
		 */
		string_free(ANGBAND_DIR_USER);
		string_free(ANGBAND_DIR_ARCHIVE);
		string_free(ANGBAND_DIR_SCORES);
		string_free(ANGBAND_DIR_SAVE);
		string_free(ANGBAND_DIR_PANIC);

		path_build(configpath, sizeof(configpath), libpath, "user");
		ANGBAND_DIR_USER = string_make(configpath);

		path_build(datapath, sizeof(datapath), ANGBAND_DIR_USER, "archive");
		ANGBAND_DIR_ARCHIVE = string_make(datapath);
		path_build(datapath, sizeof(datapath), ANGBAND_DIR_USER, "scores");
		ANGBAND_DIR_SCORES = string_make(datapath);
		path_build(datapath, sizeof(datapath), ANGBAND_DIR_USER, "save");
		ANGBAND_DIR_SAVE = string_make(datapath);
		path_build(datapath, sizeof(datapath), ANGBAND_DIR_USER, "panic");
		ANGBAND_DIR_PANIC = string_make(datapath);

		printf("borg: user=%s save=%s\n", ANGBAND_DIR_USER, ANGBAND_DIR_SAVE);
	}

	strnfmt(path, sizeof(path), "%s.keys", opt_out);
	keys_file = fopen(path, opt_replay ? "r" : "w");
	if (!opt_replay && !keys_file) {
		printf("borg: cannot write %s: %s\n", path, strerror(errno));
		return 2;
	}
	if (opt_replay && keys_file) { fclose(keys_file); keys_file = NULL; }

	strnfmt(path, sizeof(path), "%s.sync", opt_out);
	sync_file = fopen(path, "w");
	if (!sync_file) {
		printf("borg: cannot write %s: %s\n", path, strerror(errno));
		return 2;
	}

	if (opt_replay && !replay_load(opt_replay)) return 2;

	savefile_set_name(opt_savefile, true, false);
	create_needed_dirs();

	ANGBAND_SYS = "borg";
	term_data_link_borg();
	signals_init(false, true);
	cmd_get_hook = textui_get_cmd;

	init_display();
	init_angband();
	textui_init();

	/*
	 * Capture the borg's own hook BEFORE anything is played.
	 *
	 * borg_inkey_hack() is static inside borg.c and there is no other way to name it;
	 * borg_update_entrypoint() is the accessor, and asking it to install and then
	 * immediately uninstall leaves the pointer in hand with the borg untouched. Doing it
	 * here, rather than the first time the borg installs it for real, is what closes the
	 * window in which a key could be produced by an unwrapped hook and go unrecorded.
	 */
	borg_update_entrypoint(true);
	real_hack = inkey_hack;
	borg_update_entrypoint(false);

	if (opt_replay) {
		printf("borg: replaying %ld keys, cap %ld commands\n", replay_n, opt_turns);
	} else {
		printf("borg: playing savefile '%s', cap %ld commands, term %dx%d sidebar %d\n",
			opt_savefile, opt_turns, opt_wid, opt_hgt, opt_sidebar);
	}

	/*
	 * sync_file is handed to sync.c in arm_recorder(), at the moment a key source is
	 * attached, and not here -- see sync_host_set_file(). Everything this front end does
	 * to reach that point (the splash, the birth, the Ctrl-Z) therefore leaves no record,
	 * and record 1 of the file is the command driven by key 1 of the keystream.
	 */
	run_t0 = clock();
	play_game(opt_new ? GAME_NEW : GAME_LOAD);

	report_and_quit();
	return 0;
}
