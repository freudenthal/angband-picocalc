// PORT: port-written for tinyrogue-pico, stage 020.
//
// The PicoCalc keyboard, as raw press/release events.
//
// Picoware's own keyboard.c was NOT vendored. It collapses every event into a single
// char: it folds Shift and Ctrl into the character and then throws the modifier away in
// a file-private `static bool key_shift`. The game needs the modifier separately --
// Player::getAction reads sf::Keyboard::isKeyPressed(LShift) to tell `,` from `<` -- so
// a driver that discards it cannot be shimmed around. That is the same shape of problem
// that cost Mothpad its legacy target (../mothpad-pico1/specifications.md 6).
//
// This layer keeps the (state, code) pair the south bridge actually reports and lets
// src/platform/input.cpp decide what it means. southbridge.c IS vendored, unmodified;
// it is the I2C transport and nothing more.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Key codes reported by the PicoCalc south bridge in the low byte of the key word.
// Printable keys report their ASCII code. These are the rest.
#define KEY_MOD_ALT   (0xA1)
#define KEY_MOD_SHL   (0xA2)
#define KEY_MOD_SHR   (0xA3)
#define KEY_MOD_SYM   (0xA4)
#define KEY_MOD_CTRL  (0xA5)

#define KEY_BACKSPACE (0x08)
#define KEY_TAB       (0x09)
#define KEY_ENTER     (0x0A)
#define KEY_RETURN    (0x0D)
#define KEY_SPACE     (0x20)

#define KEY_ESC       (0xB1)
#define KEY_LEFT      (0xB4)
#define KEY_UP        (0xB5)
#define KEY_DOWN      (0xB6)
#define KEY_RIGHT     (0xB7)

#define KEY_BREAK     (0xD0)
#define KEY_INSERT    (0xD1)
#define KEY_HOME      (0xD2)
#define KEY_DEL       (0xD4)
#define KEY_END       (0xD5)
#define KEY_PAGE_UP   (0xD6)
#define KEY_PAGE_DOWN (0xD7)

#define KEY_CAPS_LOCK (0xC1)

#define KEY_F1 (0x81)
#define KEY_F2 (0x82)
#define KEY_F3 (0x83)
#define KEY_F4 (0x84)
#define KEY_F5 (0x85)
#define KEY_F6 (0x86)
#define KEY_F7 (0x87)
#define KEY_F8 (0x88)
#define KEY_F9 (0x89)
#define KEY_F10 (0x90)

// State, from the high byte of the key word.
#define KEY_STATE_IDLE     (0)
#define KEY_STATE_PRESSED  (1)
#define KEY_STATE_HOLD     (2)
#define KEY_STATE_RELEASED (3)

#define KBD_QUEUE_SIZE (16)   // power of two

#ifdef __cplusplus
extern "C" {
#endif

// One key-down, with the modifier latch sampled at the moment it was queued.
// Latching matters: the queue is drained later, by which time Shift may be released.
typedef struct
{
    uint8_t code;     // scan code, as above
    bool    shift;    // either Shift was held, or the code is itself a shifted symbol
    bool    ctrl;
    bool    alt;
} kbd_event_t;

void keyboard_init(void);

// Read one word from the south bridge and fold it into the queue and the modifier
// latches. Call it from the main loop; there is no background timer, on purpose --
// the vendored transport does blocking I2C, which does not belong in an alarm IRQ.
void keyboard_poll(void);

bool keyboard_next(kbd_event_t *out);   // false when the queue is empty

// Live modifier state, for callers that want it outside an event.
bool keyboard_shift_held(void);
bool keyboard_ctrl_held(void);
bool keyboard_alt_held(void);

#ifdef __cplusplus
}
#endif
