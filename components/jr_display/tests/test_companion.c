/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_display/companion.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        if (failures <= 24U) { \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
        } \
    } \
} while (0)

static jr_companion_input_t input_default(jr_companion_mode_t mode)
{
    const jr_companion_input_t input = {
        .requested = mode, .idle = true, .usb_present = true,
        .privacy_muted = true, .time_valid = true, .hour = 23, .minute = 59,
        .night_start_hour = 22, .night_end_hour = 7,
        .battery_percent = 65, .next_state = JR_COMPANION_NEXT_UNKNOWN,
    };
    return input;
}

static void check_geometry(const jr_companion_view_t *view)
{
    for (unsigned i = 0; i < JR_COMPANION_LINE_COUNT; ++i) {
        const jr_companion_line_t *line = &view->lines[i];
        const size_t n = strlen(line->text);
        CHECK(n <= JR_COMPANION_ITEM_CHARS);
        if (n == 0U) {
            CHECK(line->scale == 0U && line->x == 0 && line->y == 0);
            continue;
        }
        CHECK(line->scale >= 1U && line->scale <= 3U);
        const int right = line->x + (int)(n * 6U * line->scale) - 1;
        const int bottom = line->y + 7 * line->scale - 1;
        const int corners[4][2] = {
            {line->x, line->y}, {right, line->y},
            {line->x, bottom}, {right, bottom},
        };
        for (unsigned c = 0; c < 4U; ++c) {
            const int dx = corners[c][0] - 232;
            const int dy = corners[c][1] - 232;
            CHECK(dx * dx + dy * dy <= 168 * 168);
            CHECK(corners[c][0] >= 0 && corners[c][0] < 466);
            CHECK(corners[c][1] >= 0 && corners[c][1] < 466);
        }
    }
}

static void test_policy(void)
{
    jr_companion_view_t view;
    const jr_companion_view_t off = {0};
    CHECK(!jr_companion_build(NULL, NULL));
    memset(&view, 0xa5, sizeof view);
    CHECK(!jr_companion_build(NULL, &view));
    CHECK(memcmp(&view, &off, sizeof view) == 0);

    const jr_companion_mode_t requests[] = {
        JR_COMPANION_OFF, JR_COMPANION_NIGHTSTAND, JR_COMPANION_GLANCE,
        (jr_companion_mode_t)-1, (jr_companion_mode_t)3,
    };
    for (unsigned r = 0; r < sizeof requests / sizeof requests[0]; ++r) {
        for (unsigned blockers = 0; blockers < 64U; ++blockers) {
            for (unsigned usb = 0; usb < 2U; ++usb) {
                for (unsigned privacy = 0; privacy < 2U; ++privacy) {
                    jr_companion_input_t input = input_default(requests[r]);
                    input.idle = !(blockers & 1U);
                    input.foreground = (blockers & 2U) != 0;
                    input.operator_active = (blockers & 4U) != 0;
                    input.ota_active = (blockers & 8U) != 0;
                    input.probation = (blockers & 16U) != 0;
                    /* bit5 is independently unrelated to eligibility */
                    input.charging = (blockers & 32U) != 0;
                    input.usb_present = usb != 0;
                    input.privacy_muted = privacy != 0;
                    const bool allowed_request = r == 1U || r == 2U;
                    const bool accepted = allowed_request &&
                        (blockers & 31U) == 0U && (r != 1U || usb != 0U);
                    memset(&view, 0xa5, sizeof view);
                    CHECK(jr_companion_build(&input, &view) == accepted);
                    if (!accepted) {
                        CHECK(memcmp(&view, &off, sizeof view) == 0);
                    } else {
                        CHECK(view.mode == requests[r]);
                        CHECK(view.privacy_muted == (privacy != 0U));
                        CHECK(view.charging == input.charging);
                        CHECK(view.background_rgb565 == 0x0000U);
                        CHECK(view.ink_rgb565 == 0x2104U);
                        check_geometry(&view);
                    }
                }
            }
        }
    }
    jr_companion_input_t input = input_default(JR_COMPANION_NIGHTSTAND);
    CHECK(jr_companion_build(&input, &view));
    input.usb_present = false;
    CHECK(!jr_companion_build(&input, &view));
    CHECK(memcmp(&view, &off, sizeof view) == 0);
    CHECK(!jr_companion_build(&input, NULL));
}

static void test_clock_and_night(void)
{
    jr_companion_view_t view;
    jr_companion_input_t input = input_default(JR_COMPANION_NIGHTSTAND);
    for (unsigned hour = 0; hour < 24U; ++hour) {
        for (unsigned minute = 0; minute < 60U; ++minute) {
            input.hour = (uint8_t)hour;
            input.minute = (uint8_t)minute;
            char expected[6];
            (void)snprintf(expected, sizeof expected, "%02u:%02u", hour, minute);
            CHECK(jr_companion_build(&input, &view));
            CHECK(strcmp(view.lines[0].text, expected) == 0);
        }
    }

    /* Independent circular set construction, including equal disabled bounds. */
    for (unsigned start = 0; start < 24U; ++start) {
        for (unsigned end = 0; end < 24U; ++end) {
            bool expected[24] = {false};
            for (unsigned h = start; h != end; h = (h + 1U) % 24U) {
                expected[h] = true;
            }
            input.night_start_hour = (uint8_t)start;
            input.night_end_hour = (uint8_t)end;
            for (unsigned hour = 0; hour < 24U; ++hour) {
                input.hour = (uint8_t)hour;
                input.requested = JR_COMPANION_NIGHTSTAND;
                CHECK(jr_companion_build(&input, &view));
                CHECK(view.night == expected[hour]);
                input.requested = JR_COMPANION_GLANCE;
                CHECK(jr_companion_build(&input, &view));
                CHECK(!view.night);
            }
        }
    }

    input = input_default(JR_COMPANION_NIGHTSTAND);
    input.time_valid = false;
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[0].text, "--:--") == 0 && !view.night);
    input.time_valid = true;
    input.hour = 24;
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[0].text, "--:--") == 0 && !view.night);
    input.hour = 255;
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[0].text, "--:--") == 0 && !view.night);
    input.hour = 23;
    input.minute = 60;
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[0].text, "--:--") == 0 && !view.night);
    input.minute = 255;
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[0].text, "--:--") == 0 && !view.night);
    input.minute = 59;
    input.hour = 0;
    input.night_start_hour = 24;
    CHECK(jr_companion_build(&input, &view));
    CHECK(!view.night);
    input.hour = 23;
    input.night_start_hour = 22;
    input.night_end_hour = 24;
    CHECK(jr_companion_build(&input, &view));
    CHECK(!view.night);
}

static void test_future_item(void)
{
    jr_companion_input_t input = input_default(JR_COMPANION_GLANCE);
    jr_companion_view_t view;
    const jr_companion_next_state_t states[] = {
        JR_COMPANION_NEXT_UNKNOWN, JR_COMPANION_NEXT_NONE,
        JR_COMPANION_NEXT_VALID, JR_COMPANION_NEXT_STALE,
        (jr_companion_next_state_t)99,
    };
    const char *expected[] = {
        "NEXT UNAVAILABLE", "NO NEXT ITEM", "14:30 REVIEW",
        "NEXT STALE", "NEXT UNAVAILABLE",
    };
    memcpy(input.next_item, "14:30 review", sizeof "14:30 review");
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; ++i) {
        input.next_state = states[i];
        CHECK(jr_companion_build(&input, &view));
        CHECK(strcmp(view.lines[1].text, expected[i]) == 0);
        check_geometry(&view);
        input.requested = JR_COMPANION_NIGHTSTAND;
        CHECK(jr_companion_build(&input, &view));
        CHECK(view.lines[1].text[0] == '\0');
        input.requested = JR_COMPANION_GLANCE;
    }
    input.next_state = JR_COMPANION_NEXT_VALID;
    memset(input.next_item, 'x', sizeof input.next_item);
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[1].text, "XXXXXXXXXXXXXXXXXXXXXXXX") == 0);
    check_geometry(&view);
    memcpy(input.next_item, "a\n#=\xff", 6U);
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[1].text, "A????") == 0);
    memcpy(input.next_item, " -_.:/%?!+,'()&*", sizeof " -_.:/%?!+,'()&*");
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[1].text, " -_.:/%?!+,'()&*") == 0);
    input.next_item[0] = '\0';
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[1].text, "NEXT UNAVAILABLE") == 0);
    memset(input.next_item, ' ', sizeof input.next_item);
    CHECK(jr_companion_build(&input, &view));
    CHECK(strcmp(view.lines[1].text, "NEXT UNAVAILABLE") == 0);
}

static void test_readable_layout(void)
{
    jr_companion_input_t input = input_default(JR_COMPANION_NIGHTSTAND);
    jr_companion_view_t view;
    CHECK(jr_companion_build(&input, &view));
    /* The clock is the focal, readable 21-pixel headline, centered at rest. */
    CHECK(view.lines[0].scale == 3U);
    const int mid_x2 = 2 * view.lines[0].x + 5 * 6 * view.lines[0].scale - 1;
    const int mid_y2 = 2 * view.lines[0].y + 7 * view.lines[0].scale - 1;
    CHECK(mid_x2 >= 462 && mid_x2 <= 466);
    CHECK(mid_y2 >= 462 && mid_y2 <= 466);
    input.requested = JR_COMPANION_GLANCE;
    input.charging = true;
    CHECK(jr_companion_build(&input, &view));
    CHECK(view.lines[0].scale == 3U);
    for (unsigned i = 0; i + 1U < JR_COMPANION_LINE_COUNT; ++i) {
        CHECK(view.lines[i].text[0] != '\0');
        CHECK(view.lines[i].y + 7 * view.lines[i].scale < view.lines[i + 1U].y);
    }
}

static void test_battery_charge(void)
{
    jr_companion_input_t input = input_default(JR_COMPANION_GLANCE);
    jr_companion_view_t view;
    for (unsigned percent = 0; percent <= 255U; ++percent) {
        input.battery_percent = (uint8_t)percent;
        CHECK(jr_companion_build(&input, &view));
        char expected[25] = "BAT --";
        if (percent <= 100U) {
            (void)snprintf(expected, sizeof expected, "BAT %u%%", percent);
        }
        CHECK(strcmp(view.lines[2].text, expected) == 0);
        CHECK(view.battery_percent == (percent <= 100U ? percent : 255U));
        check_geometry(&view);
    }
    for (unsigned usb = 0; usb < 2U; ++usb) {
        for (unsigned charging = 0; charging < 2U; ++charging) {
            input.usb_present = usb != 0U;
            input.charging = charging != 0U;
            CHECK(jr_companion_build(&input, &view));
            CHECK(view.charging == input.charging);
            CHECK(strcmp(view.lines[3].text, charging ? "CHARGING" :
                         usb ? "USB" : "") == 0);
        }
    }
}

static void test_sparse_equality(void)
{
    jr_companion_input_t input = input_default(JR_COMPANION_NIGHTSTAND);
    jr_companion_view_t a, b;
    CHECK(jr_companion_build(&input, &a));
    CHECK(jr_companion_build(&input, &b));
    CHECK(jr_companion_view_equal(&a, &b));
    CHECK(!jr_companion_view_equal(NULL, &b));
    CHECK(!jr_companion_view_equal(&a, NULL));
    input.next_state = JR_COMPANION_NEXT_VALID;
    memcpy(input.next_item, "feed ignored at rest", sizeof "feed ignored at rest");
    CHECK(jr_companion_build(&input, &b));
    CHECK(jr_companion_view_equal(&a, &b));

#define DIFFERENT(field, value) do { b = a; b.field = value; \
    CHECK(!jr_companion_view_equal(&a, &b)); } while (0)
    DIFFERENT(mode, JR_COMPANION_GLANCE);
    DIFFERENT(night, !a.night);
    DIFFERENT(privacy_muted, !a.privacy_muted);
    DIFFERENT(charging, !a.charging);
    DIFFERENT(battery_percent, 64);
    DIFFERENT(background_rgb565, 0x0821);
    DIFFERENT(ink_rgb565, 0xFFFF);
    for (unsigned i = 0; i < JR_COMPANION_LINE_COUNT; ++i) {
        b = a;
        b.lines[i].text[0] = 'X';
        CHECK(!jr_companion_view_equal(&a, &b));
        b = a;
        ++b.lines[i].x;
        CHECK(!jr_companion_view_equal(&a, &b));
        b = a;
        ++b.lines[i].y;
        CHECK(!jr_companion_view_equal(&a, &b));
        b = a;
        ++b.lines[i].scale;
        CHECK(!jr_companion_view_equal(&a, &b));
    }
#undef DIFFERENT
    input = input_default(JR_COMPANION_GLANCE);
    CHECK(jr_companion_build(&input, &a));
    input.minute = 58;
    CHECK(jr_companion_build(&input, &b));
    CHECK(!jr_companion_view_equal(&a, &b));
    input.minute = 59;
    input.next_state = JR_COMPANION_NEXT_NONE;
    CHECK(jr_companion_build(&input, &b));
    CHECK(!jr_companion_view_equal(&a, &b));
    input.next_state = JR_COMPANION_NEXT_UNKNOWN;
    memcpy(input.next_item, "untrusted raw value", sizeof "untrusted raw value");
    CHECK(jr_companion_build(&input, &b));
    CHECK(jr_companion_view_equal(&a, &b));
    input.battery_percent = 101;
    CHECK(jr_companion_build(&input, &a));
    input.battery_percent = 255;
    CHECK(jr_companion_build(&input, &b));
    CHECK(jr_companion_view_equal(&a, &b));

    /* Padding is deliberately different; fields describe the same view. */
    memset(&b, 0xa5, sizeof b);
    b.mode = a.mode;
    b.night = a.night;
    b.privacy_muted = a.privacy_muted;
    b.charging = a.charging;
    b.battery_percent = a.battery_percent;
    b.background_rgb565 = a.background_rgb565;
    b.ink_rgb565 = a.ink_rgb565;
    for (unsigned i = 0; i < JR_COMPANION_LINE_COUNT; ++i) {
        memcpy(b.lines[i].text, a.lines[i].text, sizeof b.lines[i].text);
        b.lines[i].x = a.lines[i].x;
        b.lines[i].y = a.lines[i].y;
        b.lines[i].scale = a.lines[i].scale;
    }
    CHECK(jr_companion_view_equal(&a, &b));
}

int main(void)
{
    test_policy();
    test_clock_and_night();
    test_future_item();
    test_readable_layout();
    test_battery_charge();
    test_sparse_equality();
    if (failures == 0U) printf("companion passed (%u checks)\n", checks);
    else printf("%u failure(s) of %u checks\n", failures, checks);
    return failures ? 1 : 0;
}
