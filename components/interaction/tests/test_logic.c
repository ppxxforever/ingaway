#include <assert.h>
#include <stdio.h>
#include "../ui_logic.h"

static int rotate(input_state_t *s, const uint8_t *seq, int length, bool reverse)
{
    int count = 0;
    for (int i = 0; i < length; ++i) {
        int event = input_rotate(s, seq[i], 4, reverse);
        count += event == INPUT_CW ? 1 : event == INPUT_CCW ? -1 : 0;
    }
    return count;
}

static int button(input_state_t *s, bool down, uint32_t now)
{
    return input_button(s, down, now, 20, 800);
}

int main(void)
{
    input_state_t s = {0};
    const uint8_t cw[] = {2,3,1,0}, ccw[] = {1,3,2,0};
    const uint8_t bounce[] = {2,0,2,3,2,3,1,0};
    const uint8_t invalid[] = {2,1,0};
    const uint8_t back[] = {2,3,2,0};
    assert(rotate(&s, cw, 4, false) == 1);
    assert(rotate(&s, ccw, 4, false) == -1);
    assert(rotate(&s, cw, 4, true) == -1);
    assert(rotate(&s, bounce, 8, false) == 1);
    assert(rotate(&s, back, 4, false) == 0);
    assert(rotate(&s, invalid, 3, false) == 0);
    s = (input_state_t){0};
    assert(button(&s, true, 10) == INPUT_NONE);
    assert(button(&s, false, 15) == INPUT_NONE);
    assert(button(&s, true, 20) == INPUT_NONE);
    assert(button(&s, true, 40) == INPUT_NONE);
    assert(button(&s, false, 100) == INPUT_NONE);
    assert(button(&s, false, 120) == INPUT_CLICK);
    assert(button(&s, false, 140) == INPUT_NONE);
    assert(button(&s, true, 200) == INPUT_NONE);
    assert(button(&s, true, 220) == INPUT_NONE);
    assert(button(&s, true, 1019) == INPUT_NONE);
    assert(button(&s, true, 1020) == INPUT_LONG);
    assert(button(&s, true, 1100) == INPUT_NONE);
    assert(button(&s, false, 1200) == INPUT_NONE);
    assert(button(&s, false, 1220) == INPUT_NONE);
    /* Unsigned timestamp rollover preserves debounce intervals. */
    s = (input_state_t){0};
    assert(button(&s, true, UINT32_MAX - 9) == INPUT_NONE);
    assert(button(&s, true, 10) == INPUT_NONE);
    assert(s.pressed);

    ui_state_t ui = {0};
    assert(!ui_event(&ui, INPUT_CLICK) && ui.page == UI_INPUT_TEST);
    assert(!ui_event(&ui, INPUT_CW) && ui.count == 1);
    assert(!ui_event(&ui, INPUT_CCW) && ui.count == 0);
    ui.count = 9999;
    assert(!ui_event(&ui, INPUT_CW) && ui.count == 9999);
    assert(!ui_event(&ui, INPUT_CLICK) && ui.count == 0);
    assert(!ui_event(&ui, INPUT_LONG) && ui.page == UI_MENU);
    assert(!ui_event(&ui, INPUT_CW) && ui.selected == 1);
    assert(!ui_event(&ui, INPUT_CLICK) && ui.page == UI_WIFI_CONFIRM && !ui.confirm_yes);
    assert(!ui_event(&ui, INPUT_CLICK) && ui.page == UI_MENU);
    assert(!ui_event(&ui, INPUT_CLICK));
    assert(!ui_event(&ui, INPUT_CCW) && ui.confirm_yes);
    assert(!ui_event(&ui, INPUT_LONG) && ui.page == UI_MENU);
    assert(!ui_event(&ui, INPUT_CLICK) && !ui.confirm_yes);
    assert(!ui_event(&ui, INPUT_CW) && ui.confirm_yes);
    assert(ui_event(&ui, INPUT_CLICK));
    puts("encoder and UI tests passed");
}
