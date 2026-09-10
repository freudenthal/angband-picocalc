// PORT: port-written for tinyrogue-pico, stage 020. See keyboard.h for why Picoware's
// keyboard.c was not vendored.

#include "keyboard.h"
#include "southbridge.h"

static bool initialised = false;

static bool mod_shift = false;
static bool mod_ctrl  = false;
static bool mod_alt   = false;

static kbd_event_t queue[KBD_QUEUE_SIZE];
static volatile uint8_t q_head = 0;
static volatile uint8_t q_tail = 0;

// The south bridge sends the *shifted* character for a symbol -- press Shift and `,`
// and it reports 0x3C '<'. TinyRogue asks the other way round: Player::getAction wants
// (Comma, shift=true). These characters exist on no unshifted key, so they imply Shift.
//
// Letters are deliberately NOT in this list even though Shift produces 'A'..'Z'. Caps
// Lock produces them too (PicoCalc/Code/picocalc_keyboard/keyboard.ino:184), and shift
// is not a cosmetic flag in this game: it turns Walk into Run, and it makes PickUp,
// Search and Interact return nullptr. Implying Shift from an uppercase letter would
// break `g` and `s` for anyone with Caps Lock on. The real SHL/SHR latch covers letters.
static bool code_implies_shift(uint8_t c)
{
    switch (c)
    {
    case '<': case '>': case '?': case '"': case ':':
    case '{': case '}': case '|': case '~': case '+':
    case '_': case '!': case '@': case '#': case '$':
    case '%': case '^': case '&': case '*': case '(':
    case ')':
        return true;
    default:
        return false;
    }
}

static void push(uint8_t code)
{
    uint8_t next = (uint8_t)((q_head + 1) & (KBD_QUEUE_SIZE - 1));
    if (next == q_tail)
        return;                      // full: drop the oldest-first policy, drop this one

    queue[q_head].code  = code;
    queue[q_head].shift = mod_shift || code_implies_shift(code);
    queue[q_head].ctrl  = mod_ctrl;
    queue[q_head].alt   = mod_alt;
    q_head = next;
}

// One FIFO entry. Returns false when the south bridge had nothing to give.
static bool poll_once(void)
{
    uint16_t word  = sb_read_keyboard();
    uint8_t  state = (uint8_t)((word >> 8) & 0xFF);
    uint8_t  code  = (uint8_t)(word & 0xFF);

    if (state == KEY_STATE_IDLE)
        return false;

    if (state == KEY_STATE_PRESSED || state == KEY_STATE_HOLD)
    {
        switch (code)
        {
        case KEY_MOD_SHL:
        case KEY_MOD_SHR:  mod_shift = true;  return true;
        case KEY_MOD_CTRL: mod_ctrl  = true;  return true;
        case KEY_MOD_ALT:  mod_alt   = true;  return true;
        case KEY_CAPS_LOCK:
        case KEY_MOD_SYM:  return true;       // handled in the south bridge
        default:           break;
        }

        if (code == KEY_ENTER)
            code = KEY_RETURN;                // the firmware sends LF; the game wants CR

        push(code);
    }
    else if (state == KEY_STATE_RELEASED)
    {
        switch (code)
        {
        case KEY_MOD_SHL:
        case KEY_MOD_SHR:  mod_shift = false; break;
        case KEY_MOD_CTRL: mod_ctrl  = false; break;
        case KEY_MOD_ALT:  mod_alt   = false; break;
        default:           break;
        }
    }

    return true;
}

void keyboard_poll(void)
{
    if (!initialised || !sb_available())
        return;

    // Drain the south bridge FIFO rather than taking one entry per call. A Shift press
    // and the character it modifies are two separate FIFO entries; draining them in the
    // same call is what keeps the latch in push() correct. Bounded so a stuck south
    // bridge cannot hold the main loop.
    for (int i = 0; i < KBD_QUEUE_SIZE; ++i)
        if (!poll_once())
            break;
}

bool keyboard_next(kbd_event_t *out)
{
    if (q_head == q_tail)
        return false;

    *out = queue[q_tail];
    q_tail = (uint8_t)((q_tail + 1) & (KBD_QUEUE_SIZE - 1));
    return true;
}

bool keyboard_shift_held(void) { return mod_shift; }
bool keyboard_ctrl_held(void)  { return mod_ctrl;  }
bool keyboard_alt_held(void)   { return mod_alt;   }

void keyboard_init(void)
{
    if (initialised)
        return;

    sb_init();
    initialised = true;
}
