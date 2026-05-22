/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * CLI commands for Gemini Live capability.
 *
 *   gemini-live test    — connect, send setup, wait for setupComplete, disconnect
 *   gemini-live start   — open session (toggle on)
 *   gemini-live stop    — close session (toggle off)
 *   gemini-live text <t>— send a text turn during active session
 */
#include "cmd_cap_gemini_live.h"

#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cap_gemini_live.h"
#include "cmd_cap_gemini_live.h"
#include "esp_console.h"

static struct {
    struct arg_lit *test;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_str *text;
    struct arg_end *end;
} gl_args;

static int cmd_gemini_live(int argc, char **argv)
{
    int err = arg_parse(argc, argv, (void **)&gl_args);
    if (err != 0) {
        arg_print_errors(stderr, gl_args.end, argv[0]);
        return 1;
    }

    if (gl_args.test->count) {
        esp_err_t r = cap_gemini_live_test();
        printf("gemini-live test: %s\n", r == ESP_OK ? "PASS" : "FAIL");
        return (r == ESP_OK) ? 0 : 1;
    }

    if (gl_args.start->count) {
        esp_err_t r = cap_gemini_live_start();
        if (r != ESP_OK) {
            printf("gemini-live start failed\n");
            return 1;
        }
        printf("Gemini Live session started\n");
        return 0;
    }

    if (gl_args.stop->count) {
        esp_err_t r = cap_gemini_live_stop();
        if (r != ESP_OK) {
            printf("gemini-live stop failed\n");
            return 1;
        }
        printf("Gemini Live session stopped\n");
        return 0;
    }

    if (gl_args.text->count) {
        esp_err_t r = cap_gemini_live_send_text(gl_args.text->sval[0]);
        if (r != ESP_OK) {
            printf("gemini-live text failed (session not active?)\n");
            return 1;
        }
        return 0;
    }

    printf("Usage: gemini-live --test | --start | --stop | --text <text>\n");
    return 1;
}

void cmd_cap_gemini_live_register(void)
{
    gl_args.test  = arg_lit0(NULL, "test",  "Connect, handshake, disconnect (no audio)");
    gl_args.start = arg_lit0(NULL, "start", "Open a Gemini Live voice session");
    gl_args.stop  = arg_lit0(NULL, "stop",  "Close the active session");
    gl_args.text  = arg_str0(NULL, "text",  "<text>", "Send a text prompt");
    gl_args.end   = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command  = "gemini-live",
        .help     = "Gemini Live voice session control",
        .hint     = NULL,
        .func     = cmd_gemini_live,
        .argtable = &gl_args,
    };
    esp_console_cmd_register(&cmd);
}
