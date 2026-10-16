#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t ab;
    int partial;
    bool raw_pressed, pressed, long_sent;
    uint32_t changed_ms, pressed_ms;
} input_state_t;

enum { INPUT_NONE, INPUT_CW, INPUT_CCW, INPUT_CLICK, INPUT_LONG };

static inline int input_rotate(input_state_t *s, uint8_t ab, int transitions, bool reverse)
{
    static const int8_t table[16] = {
        0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0
    };
    if ((s->ab ^ ab) == 3) s->partial = 0;
    else s->partial += table[s->ab * 4 + ab];
    s->ab = ab;
    int step = 0;
    if (s->partial >= transitions) { step = 1; s->partial = 0; }
    if (s->partial <= -transitions) { step = -1; s->partial = 0; }
    if (reverse) step = -step;
    return step > 0 ? INPUT_CW : step < 0 ? INPUT_CCW : INPUT_NONE;
}

static inline int input_button(input_state_t *s, bool pressed, uint32_t now,
                               uint32_t debounce_ms, uint32_t long_ms)
{
    if (s->raw_pressed != pressed) {
        s->raw_pressed = pressed;
        s->changed_ms = now;
    }
    if (s->pressed != pressed && (uint32_t)(now - s->changed_ms) >= debounce_ms) {
        s->pressed = pressed;
        if (pressed) {
            s->pressed_ms = now;
            s->long_sent = false;
        } else if (!s->long_sent) return INPUT_CLICK;
    }
    if (s->pressed && pressed && !s->long_sent &&
        (uint32_t)(now - s->pressed_ms) >= long_ms) {
        s->long_sent = true;
        return INPUT_LONG;
    }
    return INPUT_NONE;
}
