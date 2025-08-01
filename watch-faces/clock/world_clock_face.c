/*
 * MIT License
 *
 * Copyright (c) 2022-2024 Joey Castillo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include "world_clock_face.h"
#include "watch.h"
#include "watch_utility.h"
#include "watch_common_display.h"
#include "filesystem.h"
#include "zones.h"

static int world_clock_instances;

static void persist_world_clock_settings(world_clock_state_t *state) {
    world_clock_settings_t maybe_settings;
    char filename[13];

    maybe_settings.reg = 0xFFFFFFFF;
    sprintf(filename, "wclk_%03d.u32", state->clock_index);

    filesystem_read_file(filename, (char *) &maybe_settings.reg, sizeof(world_clock_settings_t));
    if (state->settings.reg != maybe_settings.reg) {
        filesystem_write_file(filename, (char *) &state->settings.reg, sizeof(world_clock_settings_t));
    }
}

static void advance_character_at_position(char *character, uint8_t position) {
    bool is_custom_lcd = watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM;
    if (is_custom_lcd || position == 0) {
        // On custom LCD and classic's position 0 we support all characters.
        // All we need to do is jump around the ASCII table to the useful ones.
        switch (*character) {
            case ' ':
                *character = 'A';
                break;
            case 'z':
                *character = '0';
                break;
            case '9':
                *character = '{';
                break;
            case '}':
                *character = '*';
                break;
            case '.':
                *character = '/';
                break;
            case '/':
            case 0x7F: // failsafe: if they've broken out of the intended rotation, return them to 0x20
                *character = ' ';
                break;
            default:
                *character += 1;
                break;
        }
    } else {
        // otherwise we have to do some wacky shit
        switch (*character) {
            case ' ':
                *character = 'A';
                break;
            case 'F':
            case 'J':
            case 'L':
            case 'R':
            case '1':
                *character += 2;
                break;
            case 'H':
                *character = 'l';
                break;
            case 'l':
                *character = 'J';
                break;
            case 'O':
                *character = 'R';
                break;
            case 'U':
                *character = 'X';
                break;
            case 'X':
                *character = '0';
                break;
            case '3':
                *character = '7';
                break;
            case '8':
                *character = '{';
                break;
            case '{':
            case 0x7F: // failsafe: if they've broken out of the intended rotation, return them to 0x20
                *character = ' ';
                break;
            default:
                *character += 1;
                break;
        }
    }
}

static void _update_timezone_offset(world_clock_state_t *state) {
    state->current_offset = movement_get_current_timezone_offset_for_zone(state->settings.bit.timezone_index);
}

void world_clock_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(world_clock_state_t));
        memset(*context_ptr, 0, sizeof(world_clock_state_t));
        world_clock_state_t *state = (world_clock_state_t *)*context_ptr;
        state->clock_index = world_clock_instances++;

        // load settings from file if it exists
        char filename[13];
        sprintf(filename, "wclk_%03d.u32", state->clock_index);
        if (filesystem_file_exists(filename)) {
            filesystem_read_file(filename, (char *) &state->settings.reg, sizeof(world_clock_settings_t));
        } else {
            // otherwise make all characters blank by default, and set to UTC time
            state->settings.bit.char_0 = ' ';
            state->settings.bit.char_1 = ' ';
            state->settings.bit.char_2 = ' ';
            state->settings.bit.timezone_index = UTZ_UTC;
        }
    }
}

void world_clock_face_activate(void *context) {
    world_clock_state_t *state = (world_clock_state_t *)context;

    state->current_screen = 0;
    _update_timezone_offset(state);

    if (watch_sleep_animation_is_running()) {
        watch_stop_sleep_animation();
        watch_stop_blink();
    }
}

static bool world_clock_face_do_display_mode(movement_event_t event, world_clock_state_t *state) {
    char buf[11];

    uint32_t previous_date_time;
    watch_date_time_t date_time;
    switch (event.event_type) {
        case EVENT_ACTIVATE:
            watch_set_colon();
            state->previous_date_time = 0xFFFFFFFF;
            // fall through
        case EVENT_TICK:
        case EVENT_LOW_ENERGY_UPDATE:
            date_time = movement_get_date_time_in_zone(state->settings.bit.timezone_index);
            previous_date_time = state->previous_date_time;
            state->previous_date_time = date_time.reg;
            if ((date_time.reg >> 6) == (previous_date_time >> 6) && event.event_type != EVENT_LOW_ENERGY_UPDATE) {
                // everything before seconds is the same, don't waste cycles setting those segments.
                watch_display_character_lp_seconds('0' + date_time.unit.second / 10, 8);
                watch_display_character_lp_seconds('0' + date_time.unit.second % 10, 9);
                break;
            } else if ((date_time.reg >> 12) == (previous_date_time >> 12) && event.event_type != EVENT_LOW_ENERGY_UPDATE) {
                // everything before minutes is the same.
                sprintf(buf, "%02d%02d", date_time.unit.minute, date_time.unit.second);
                watch_display_text(WATCH_POSITION_MINUTES, buf);
                watch_display_text(WATCH_POSITION_SECONDS, buf + 2);
                if (date_time.unit.minute % 15 == 0) {
                    _update_timezone_offset(state);
                }
            } else {
                // other stuff changed; let's do it all.
                watch_display_character(state->settings.bit.char_0, 0);
                watch_display_character(state->settings.bit.char_1, 1);
                if (watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM) {
                    watch_display_character(state->settings.bit.char_2, 10);
                }
                sprintf(buf, "%2d%2d%02d%02d", date_time.unit.day, date_time.unit.hour, date_time.unit.minute, date_time.unit.second);
                watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
                watch_display_text(WATCH_POSITION_HOURS, buf + 2);
                watch_display_text(WATCH_POSITION_MINUTES, buf + 4);
                if (event.event_type == EVENT_LOW_ENERGY_UPDATE) {
                    if (!watch_sleep_animation_is_running()) {
                        watch_display_text(WATCH_POSITION_SECONDS, "  ");
                        watch_start_sleep_animation(500);
                        watch_start_indicator_blink_if_possible(WATCH_INDICATOR_COLON, 500);
                    }
                } else {
                    watch_display_text(WATCH_POSITION_SECONDS, buf + 6);
                }
            }
            break;
        case EVENT_ALARM_LONG_PRESS:
            movement_request_tick_frequency(4);
            state->current_screen = 1;
            break;
        default:
            return movement_default_loop_handler(event);
    }

    return true;
}

static bool _world_clock_face_do_settings_mode(movement_event_t event, world_clock_state_t *state) {
    bool is_custom_lcd;

    switch (event.event_type) {
        case EVENT_MODE_BUTTON_UP:
            persist_world_clock_settings(state);
            movement_move_to_next_face();
            return false;
        case EVENT_LIGHT_BUTTON_DOWN:
            state->current_screen++;
            is_custom_lcd = watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM;
            if ((is_custom_lcd && state->current_screen > 4) || (!is_custom_lcd && state->current_screen > 3)) {
                movement_request_tick_frequency(1);
                _update_timezone_offset(state);
                state->current_screen = 0;
                persist_world_clock_settings(state);
                event.event_type = EVENT_ACTIVATE;
                return world_clock_face_do_display_mode(event, state);
            }
            break;
        case EVENT_ALARM_BUTTON_DOWN:
            switch (state->current_screen) {
                case 1:
                    advance_character_at_position(&state->settings.bit.char_0, 0);
                    break;
                case 2:
                    advance_character_at_position(&state->settings.bit.char_1, 1);
                    break;
                case 3:
                    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM) {
                        advance_character_at_position(&state->settings.bit.char_2, 2);
                        break;
                    }
                    // fall through
                case 4:
                    state->settings.bit.timezone_index++;
                    if (state->settings.bit.timezone_index >= NUM_ZONE_NAMES) state->settings.bit.timezone_index = 0;
                    break;
            }
            break;
        case EVENT_TIMEOUT:
            persist_world_clock_settings(state);
            movement_move_to_face(0);
            break;
        default:
            break;
    }

    char buf[13];

    watch_clear_colon();
    sprintf(buf, "%c%c  %s%c",
        state->settings.bit.char_0,
        state->settings.bit.char_1,
        watch_utility_time_zone_name_at_index(state->settings.bit.timezone_index),
        state->settings.bit.char_2);
    watch_clear_indicator(WATCH_INDICATOR_PM);

    // blink up the parameter we're setting
    if (event.subsecond % 2) {
        switch (state->current_screen) {
            case 1:
            case 2:
                buf[state->current_screen - 1] = '_';
                break;
            case 3:
                if (watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM) {
                    buf[10] = '_';
                    break;
                }
                // fall through
            case 4:
                memcpy(buf + 4, "      ", 6);
                break;
        }
    }

    watch_display_text(WATCH_POSITION_FULL, buf);

    return true;
}

bool world_clock_face_loop(movement_event_t event, void *context) {
    world_clock_state_t *state = (world_clock_state_t *)context;

    if (state->current_screen == 0) {
        return world_clock_face_do_display_mode(event, state);
    } else {
        return _world_clock_face_do_settings_mode(event, state);
    }
}

void world_clock_face_resign(void *context) {
    (void) context;
}
