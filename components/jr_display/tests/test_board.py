#!/usr/bin/env python3
"""Compile the actual board table/sleep transport seam against a narrow IO fake.

No copy of the implementation is tested. Extracted source is persisted under
workspace evidence; vendor reset/driver internals remain target/physical proof.
"""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path,
                    default=ROOT / 'firmware/components/jarvis_board/src/jarvis_board.c')
args = parser.parse_args()
source_path = args.source.resolve()
if not source_path.is_relative_to(ROOT):
    raise SystemExit('source must stay in workspace')
source = source_path.read_text()
table = re.search(r'static const co5300_lcd_init_cmd_t s_lcd_init_cmds\[\] = \{.*?\n\};',
                  source, re.S).group()
start = source.index('esp_err_t jarvis_board_display_set_sleep(bool sleep)')
end = source.index('\nstatic SemaphoreHandle_t', start)
adapter = source[start:end]
bootstrap = re.search(r'ESP_GOTO_ON_ERROR\(esp_lcd_panel_co5300_set_brightness\(panel, \d+\),.*?\);', source, re.S).group()
# Actual board setup still binds this table and the pinned QSPI transport.
assert '.init_cmds = s_lcd_init_cmds,' in source
assert '.use_qspi_interface = 1,' in source
assert 'esp_lcd_panel_set_gap(panel, 0x06, 0)' in source
assert 'esp_lcd_panel_reset(panel)' in source
digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
scratch = (ROOT / '.evidence/t107/C-board-test' / digest).resolve()
assert scratch.is_relative_to(ROOT / '.evidence/t107')
scratch.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; } co5300_lcd_init_cmd_t;
static bool s_display_ready;
static struct { void *io; } s_display;
static unsigned calls, command;
static void *last_io;
static esp_err_t transport_result;
static bool param_invalid;
static unsigned brightness_calls, bootstrap_percent;
static esp_err_t brightness_result;
static esp_err_t esp_lcd_panel_co5300_set_brightness(void *panel, unsigned percent)
{ (void)panel; brightness_calls++; bootstrap_percent = percent; return brightness_result; }
#define ESP_GOTO_ON_ERROR(expr,label,tag,...) do { ret=(expr); if(ret != ESP_OK) goto label; } while(0)
static esp_err_t esp_lcd_panel_io_tx_param(void *io, int cmd, const void *data, size_t bytes)
{ calls++; command = (unsigned)cmd; last_io = io; param_invalid = data != NULL || bytes != 0; return transport_result; }
'''
harness += table + '\n' + adapter
harness += '\nstatic esp_err_t board_bootstrap_brightness(void *panel) { esp_err_t ret;\n' + bootstrap + '\nreturn ESP_OK; fail_display: return ret; }\n'
harness += r'''
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line%d: %s\n", __LINE__, #x); } } while (0)
int main(void)
{
 const uint8_t ops[] = {0xfe,0x19,0x1c,0xfe,0xc4,0x3a,0x35,0x53,0x51,0x63,0x2a,0x2b,0x11,0x29};
 const uint8_t bytes[][4] = {{0x20},{0x10},{0xa0},{0},{0x80},{0x55},{0},{0x20},{0},{0xff},
                            {0,6,1,0xd7},{0,0,1,0xd1},{0},{0}};
 CHECK(sizeof s_lcd_init_cmds / sizeof s_lcd_init_cmds[0] == sizeof ops);
 unsigned wait = 0;
 for (unsigned i = 0; i < sizeof ops; ++i) {
   const co5300_lcd_init_cmd_t *p = &s_lcd_init_cmds[i];
   const unsigned n = i >= 12 ? 0 : i >= 10 ? 4 : 1;
   CHECK(p->cmd == ops[i]); CHECK(p->data_bytes == n);
   CHECK(n ? memcmp(p->data, bytes[i], n) == 0 : p->data == NULL);
   CHECK(p->delay_ms == (i == 12 ? 120 : 0)); wait += p->delay_ms;
 }
 CHECK(wait == 120);
 CHECK(board_bootstrap_brightness((void *)1) == ESP_OK);
 CHECK(brightness_calls == 1 && bootstrap_percent == 0);
 brightness_result = ESP_FAIL;
 CHECK(board_bootstrap_brightness((void *)1) == ESP_FAIL);
 CHECK(brightness_calls == 2 && bootstrap_percent == 0);
 CHECK(jarvis_board_display_set_sleep(true) == ESP_ERR_INVALID_STATE && calls == 0);
 s_display.io = (void *)1;
 CHECK(jarvis_board_display_set_sleep(false) == ESP_ERR_INVALID_STATE && calls == 0);
 s_display.io = NULL;
 s_display_ready = true;
 CHECK(jarvis_board_display_set_sleep(false) == ESP_ERR_INVALID_STATE && calls == 0);
 s_display.io = (void *)1;
 CHECK(jarvis_board_display_set_sleep(true) == ESP_OK);
 CHECK(calls == 1 && command == 0x02001000 && last_io == s_display.io && !param_invalid);
 CHECK(jarvis_board_display_set_sleep(false) == ESP_OK);
 CHECK(calls == 2 && command == 0x02001100 && last_io == s_display.io && !param_invalid);
 transport_result = ESP_FAIL;
 CHECK(jarvis_board_display_set_sleep(true) == ESP_FAIL);
 CHECK(jarvis_board_display_set_sleep(false) == ESP_FAIL);
 CHECK(calls == 4);
 printf("board source tests: %u checks, %u failures\n", checks, failures);
 return failures ? 1 : 0;
}
'''
(scratch / 'board-seam.c').write_text(harness)
build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(scratch / 'board-seam.c'), '-o', str(scratch / 'test')],
                       capture_output=True, text=True)
(scratch / 'compile.log').write_text(build.stdout + build.stderr)
if build.returncode:
    print(build.stdout + build.stderr)
    raise SystemExit(build.returncode)
run = subprocess.run([str(scratch / 'test')], capture_output=True, text=True)
(scratch / 'run.log').write_text(run.stdout + run.stderr)
print(run.stdout + run.stderr, end='')
if run.returncode == 0:
    count = re.search(r'board source tests: (\d+) checks, 0 failures', run.stdout).group(1)
    print(f'board source passed ({count} checks)')
raise SystemExit(run.returncode)
