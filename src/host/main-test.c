/**
 * \file main-test.c
 * \brief Pseudo-UI for end-to-end testing.
 *
 * Copyright (c) 2011 Elly <elly+angband@leptoquark.net>
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "buildid.h"
/* PORT: cmd-core.h, for the CMD_WIZ_JUMP_LEVEL push in c_jump() below. */
#include "cmd-core.h"
#include "main.h"
#include "player.h"
#include "player-birth.h"
/* PORT: stage 060, for do_cmd_redraw() in c_sidebar(). */
#include "ui-command.h"
#include "ui-game.h"

#ifdef USE_TEST

static int prompt = 0;
static int verbose = 0;
static int nextkey = 0;

static void c_key(char *rest) {
	if (streq(rest, "left")) {
		nextkey = ARROW_LEFT;
	} else if (streq(rest, "right")) {
		nextkey = ARROW_RIGHT;
	} else if (streq(rest, "up")) {
		nextkey = ARROW_UP;
	} else if (streq(rest, "down")) {
		nextkey = ARROW_DOWN;
	} else if (streq(rest, "space")) {
		nextkey = ' ';
	} else if (streq(rest, "enter")) {
		nextkey = '\n';
	/* PORT: stage 060. ESCAPE leaves every screen the layout sweep visits. */
	} else if (streq(rest, "escape") || streq(rest, "esc")) {
		nextkey = ESCAPE;
	/* PORT: end */
	} else if (rest[0] == 'C' && rest[1] == '-') {
		nextkey = KTRL(rest[2]);
	} else {
		nextkey = rest[0];
	}
}

static void c_noop(char *rest) {

}

static void c_quit(char *rest) {
	quit(NULL);
}

static void c_verbose(char *rest) {
	if (rest && streq(rest, "0")) {
		printf("cmd-verbose: off\n");
		verbose = 0;
	} else {
		printf("cmd-verbose: on\n");
		verbose = 1;
	}
}

static void c_version(char *rest) {
	printf("cmd-version: %s\n", buildid);
}

/**
 * Player commands
 */
static void c_player_birth(char *rest) {
	const char *race = strtok(rest, " ");
	const char *class = strtok(NULL, " ");
	struct player_class *c;
	struct player_race *r;

	if (!race) race = "Human";
	if (!class) class = "Warrior";

	for (r = races; r; r = r->next)
		if (streq(race, r->name))
			break;
	if (!r) {
		printf("player-birth: bad race '%s'\n", race);
		return;
	}

	for (c = classes; c; c = c->next)
		if (streq(class, c->name))
			break;

	if (!c) {
		printf("player-birth: bad class '%s'\n", class);
		return;
	}

	player_generate(player, r, c, false);
}

static void c_player_class(char *rest) {
	printf("player-class: %s\n", player->class->name);
}

static void c_player_race(char *rest) {
	printf("player-race: %s\n", player->race->name);
}

/*
 * PORT: three probes for the port's host harness (tools/host-build.sh). They exist to
 * measure the heap table in specifications.md section 7.1 without a device, and are
 * compiled only into the host build; src/host/ is not part of the device firmware.
 *
 *   heap?  [tag]  ask tools/heapshim.c for live/peak/allocation counts. The symbol is
 *                 weak, so the binary still runs without LD_PRELOAD.
 *   depth?        print the current dungeon level.
 *   jump N        queue a wizard level jump, bypassing the Ctrl-A confirmation prompt.
 */
extern void heapshim_report(const char *tag) __attribute__((weak));

static void c_heap(char *rest) {
	if (heapshim_report) {
		heapshim_report(rest ? rest : "?");
	} else {
		printf("heap: no shim\n");
	}
}

static void c_depth(char *rest) {
	printf("depth: %d\n", player->depth);
}

static void c_jump(char *rest) {
	int n = rest ? atoi(rest) : 1;

	cmdq_push(CMD_WIZ_JUMP_LEVEL);
	cmd_set_arg_number(cmdq_peek(), "level", n);
	cmd_set_arg_choice(cmdq_peek(), "choice", 0);
	printf("jump: queued %d\n", n);
}

/*
 * PORT: stage 060. Three more probes, for the 64-column layout work.
 *
 *   screen? [tag]  dump the whole Term grid as text, one "SCR|" line per row, trailing
 *                  blanks stripped. This is the capture the stage's run log is written
 *                  from: a screen that has been read but not dumped is not a record.
 *                  Non-ASCII cells print as '?' -- the device folds them to ASCII anyway
 *                  (specifications.md section 6.4) and this harness has no font.
 *   sidebar N      set angband_term[0]->sidebar_mode (0 Left, 1 Top, 2 None) and force a
 *                  full redraw, so a script can compare the three without the options menu.
 *   size?          print the term size, so a capture carries the geometry it was taken at.
 *
 * The term size itself comes from "-s WxH" on the -mtest argument list; it defaults to
 * 80x24 so every pre-existing test under tests/ is unaffected.
 */
static void c_screen(char *rest) {
	int w, h, x, y;
	char line[256];

	Term_get_size(&w, &h);
	printf("screen: %s %dx%d\n", rest ? rest : "-", w, h);

	for (y = 0; y < h; y++) {
		int last = -1;

		for (x = 0; x < w && x < (int)sizeof(line) - 1; x++) {
			int a = 0;
			wchar_t c = 0;

			if (Term_what(x, y, &a, &c) != 0) c = L' ';
			line[x] = (c >= 32 && c < 127) ? (char)c : (c == 0 || c == L' ') ? ' ' : '?';
			if (line[x] != ' ') last = x;
		}
		line[last + 1] = '\0';
		printf("SCR|%s|\n", line);
	}
	printf("screen: end\n");
}

static void c_sidebar(char *rest) {
	int n = rest ? atoi(rest) : 0;

	if (n < 0 || n >= SIDEBAR_MAX) n = SIDEBAR_LEFT;
	SIDEBAR_MODE = n;
	printf("sidebar: %d\n", n);
	/*
	 * Setting the mode is not enough: ui-options.c's own 'o' command relies on the
	 * screen_load() after it to repaint, and nothing here does that. do_cmd_redraw()
	 * is what Ctrl-R runs and is the same path.
	 */
	if (player && player->upkeep && player->upkeep->playing) {
		do_cmd_redraw();
	}
}

static void c_size(char *rest) {
	int w, h;

	Term_get_size(&w, &h);
	printf("size: %dx%d\n", w, h);
}
/* PORT: end */

typedef struct {
	const char *name;
	void (*func)(char *args);
} test_cmd;

static test_cmd cmds[] = {
	{ "#", c_noop },
	{ "key", c_key },
	{ "noop", c_noop },
	{ "quit", c_quit },
	{ "verbose", c_verbose },
	{ "version?", c_version },


	/* PORT: harness probes, see above. */
	{ "heap?", c_heap },
	{ "depth?", c_depth },
	{ "jump", c_jump },
	/* PORT: stage 060 */
	{ "screen?", c_screen },
	{ "sidebar", c_sidebar },
	{ "size?", c_size },
	/* PORT: end */

	{ "player-birth", c_player_birth },
	{ "player-class?", c_player_class },
	{ "player-race?", c_player_race },

	{ NULL, NULL }
};

static errr test_docmd(void) {
	char buf[1024];
	char *cmd;
	char *rest;
	int i;

	memset(buf, 0, sizeof(buf));

	if (prompt) {
		printf("test> ");
		fflush(stdout);
	}
	if (!fgets(buf, sizeof(buf), stdin)) {
		return -1;
	}
	if (strchr(buf, '\n')) {
		*strchr(buf, '\n') = '\0';
	}

	if (verbose) printf("test-docmd: %s\n", buf);
	cmd = strtok(buf, " ");
	if (!cmd) return 0;
	rest = strtok(NULL, "");

	for (i = 0; cmds[i].name; i++) {
		if (streq(cmds[i].name, cmd)) {
			cmds[i].func(rest);
			return 0;
		}
	}

	return 0;
}

typedef struct term_data term_data;
struct term_data {
	term t;
};

static term_data td;
typedef struct {
	int key;
	errr (*func)(int v);
} term_xtra_func;

static void term_init_test(term *t) {
	if (verbose) printf("term-init %s\n", buildid);
}

static void term_nuke_test(term *t) {
	if (verbose) printf("term-end\n");
}

static errr term_xtra_clear(int v) {
	if (verbose) printf("term-xtra-clear %d\n", v);
	return 0;
}

static errr term_xtra_noise(int v) {
	if (verbose) printf("term-xtra-noise %d\n", v);
	return 0;
}

static errr term_xtra_fresh(int v) {
	if (verbose) printf("term-xtra-fresh %d\n", v);
	return 0;
}

static errr term_xtra_shape(int v) {
	if (verbose) printf("term-xtra-shape %d\n", v);
	return 0;
}

static errr term_xtra_alive(int v) {
	if (verbose) printf("term-xtra-alive %d\n", v);
	return 0;
}

static errr term_xtra_event(int v) {
	if (verbose) printf("term-xtra-event %d\n", v);
	if (nextkey) {
		Term_keypress(nextkey, 0);
		nextkey = 0;
	}
	return test_docmd();
}

static errr term_xtra_flush(int v) {
	if (verbose) printf("term-xtra-flush %d\n", v);
	return 0;
}

static errr term_xtra_delay(int v) {
	if (verbose) printf("term-xtra-delay %d\n", v);
	return 0;
}

static errr term_xtra_react(int v) {
	if (verbose) printf("term-xtra-react\n");
	return 0;
}

static term_xtra_func xtras[] = {
	{ TERM_XTRA_CLEAR, term_xtra_clear },
	{ TERM_XTRA_NOISE, term_xtra_noise },
	{ TERM_XTRA_FRESH, term_xtra_fresh },
	{ TERM_XTRA_SHAPE, term_xtra_shape },
	{ TERM_XTRA_ALIVE, term_xtra_alive },
	{ TERM_XTRA_EVENT, term_xtra_event },
	{ TERM_XTRA_FLUSH, term_xtra_flush },
	{ TERM_XTRA_DELAY, term_xtra_delay },
	{ TERM_XTRA_REACT, term_xtra_react },
	{ 0, NULL },
};

static errr term_xtra_test(int n, int v) {
	int i;
	for (i = 0; xtras[i].func; i++) {
		if (xtras[i].key == n) {
			return xtras[i].func(v);
		}
	}
	if (verbose) printf("term-xtra-unknown %d %d\n", n, v);
	return 0;
}

static errr term_curs_test(int x, int y) {
	if (verbose) printf("term-curs %d %d\n", x, y);
	return 0;
}

static errr term_wipe_test(int x, int y, int n) {
	if (verbose) printf("term-wipe %d %d %d\n", x, y, n);
	return 0;
}

static errr term_text_test(int x, int y, int n, int a, const wchar_t *s) {
	if (verbose) {
		char str[256];
		wcstombs(str, s, 256);
		printf("term-text %d %d %d %02x %s\n", x, y, n, a, str);
	}
	return 0;
}

/*
 * PORT: stage 060. The term geometry, set by "-s WxH". 80x24 is upstream's and is what
 * every test under tests/ runs at; the port's device term is 64x32.
 */
static int term_wid = 80;
static int term_hgt = 24;

/* PORT: stage 060, "-b N" sets the initial sidebar mode; the device's default is 1. */
static int term_sidebar = SIDEBAR_LEFT;

static void term_data_link(int i) {
	term *t = &td.t;

	term_init(t, term_wid, term_hgt, 256);
	/* PORT: stage 060. term_init() leaves this at SIDEBAR_LEFT; "-b N" overrides it. */
	t->sidebar_mode = term_sidebar;

	t->init_hook = term_init_test;
	t->nuke_hook = term_nuke_test;

	t->xtra_hook = term_xtra_test;
	t->curs_hook = term_curs_test;
	t->wipe_hook = term_wipe_test;
	t->text_hook = term_text_test;

	t->data = &td;

	Term_activate(t);

	angband_term[i] = t;
}

const char help_test[] = "Test mode, subopts -p(rompt) -s(ize) WxH -b(sidebar) N";

errr init_test(int argc, char *argv[]) {
	int i;

	/* Skip over argv[0] */
	for (i = 1; i < argc; i++) {
		if (streq(argv[i], "-p")) {
			prompt = 1;
			continue;
		}
		/* PORT: stage 060, "-s WxH" sets the term geometry. */
		if (prefix(argv[i], "-s")) {
			int w = 0, h = 0;

			if (sscanf(argv[i] + 2, "%dx%d", &w, &h) == 2 &&
					w >= 20 && w <= 255 && h >= 10 && h <= 255) {
				term_wid = w;
				term_hgt = h;
				continue;
			}
			printf("init-test: bad size '%s'\n", argv[i]);
			continue;
		}
		/* PORT: stage 060, "-b N" sets the initial sidebar mode. */
		if (prefix(argv[i], "-b")) {
			int b = atoi(argv[i] + 2);

			if (b >= 0 && b < SIDEBAR_MAX) {
				term_sidebar = b;
				continue;
			}
			printf("init-test: bad sidebar mode '%s'\n", argv[i]);
			continue;
		}
		/* PORT: end */
		printf("init-test: bad argument '%s'\n", argv[i]);
	}

	/*
	 * Reset savefile set by main.c:  don't want it to interfere with the
	 * test.
	 */
	savefile[0] = '\0';

	term_data_link(0);
	return 0;
}
#endif
