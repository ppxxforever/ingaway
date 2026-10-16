#pragma once
#include <stdbool.h>
#include "input_logic.h"

enum { UI_MENU, UI_INPUT_TEST, UI_WIFI_CONFIRM };
typedef struct { int page, selected, count; bool confirm_yes; } ui_state_t;

/* Returns true only after entering confirmation, choosing YES, then clicking. */
static inline bool ui_event(ui_state_t *s, int event)
{
    if (event == INPUT_LONG) {
        s->page = UI_MENU;
        s->confirm_yes = false;
    } else if (event == INPUT_CW || event == INPUT_CCW) {
        int direction = event == INPUT_CW ? 1 : -1;
        if (s->page == UI_MENU) s->selected = 1 - s->selected;
        else if (s->page == UI_WIFI_CONFIRM) s->confirm_yes = !s->confirm_yes;
        else {
            /* Bound the diagnostic count (avoid signed overflow). */
            s->count += direction;
            if (s->count > 9999) s->count = 9999;
            if (s->count < -9999) s->count = -9999;
        }
    } else if (event == INPUT_CLICK) {
        if (s->page == UI_MENU) {
            s->page = s->selected == 0 ? UI_INPUT_TEST : UI_WIFI_CONFIRM;
            s->confirm_yes = false;
        } else if (s->page == UI_INPUT_TEST) s->count = 0;
        else if (s->confirm_yes) return true;
        else s->page = UI_MENU;
    }
    return false;
}
