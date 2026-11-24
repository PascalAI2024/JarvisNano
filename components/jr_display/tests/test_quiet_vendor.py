#!/usr/bin/env python3
"""Compile pinned vendor playback functions with the actual quiet entry commands.

Frame parsing and hardware IO are fakes; playback control/callback and the
production stop/segment/start/visibility sequence are extracted, not copied.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--display-source', type=Path,
                    default=root / 'components/jr_display/src/jr_display.c')
parser.add_argument('--vendor-source', type=Path,
                    default=root / 'managed_components/espressif2022__esp_emote_gfx/src/widget/gfx_anim.c')
parser.add_argument('--skip-patch-conformance', action='store_true')
parser.add_argument('--fixture-only', action='store_true',
                    help='prove the fresh-checkout path without managed components')
args = parser.parse_args()
display_path = args.display_source.resolve()
assert display_path.is_relative_to(root)
vendor_path = args.vendor_source.resolve()
assert vendor_path.is_relative_to(root)
display = display_path.read_text()
spec = importlib.util.spec_from_file_location('managed_patch', root / 'scripts/patch-v5-managed.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

def function(source, signature):
    a = source.index(signature + '\n{')
    b = source.index('\n}', a) + 2
    return source[a:b]

signatures = [
    'static void gfx_anim_timer_callback(void *arg)',
    'esp_err_t gfx_anim_set_segment(gfx_obj_t *obj, uint32_t start, uint32_t end, uint32_t fps, bool repeat)',
    'esp_err_t gfx_anim_start(gfx_obj_t *obj)',
    'esp_err_t gfx_anim_stop(gfx_obj_t *obj)']
fixture_path = root / 'components/jr_display/tests/fixtures/gfx_anim_playback_3_0_2.c'
fixture = fixture_path.read_text()
# This is the same transform called by the build patch script, then compiled
# below. Idempotence and refusal of changed/partial anchors stay observable.
patched_fixture = patch.apply_frame_retry(fixture)
assert patch.apply_frame_retry(patched_fixture) == patched_fixture
for invalid in [fixture.replace(patch.FRAME_RETRY_ANCHOR, ''),
                patched_fixture.replace('            return;\n        }\n        anim->current_frame++;',
                                        '        }\n        anim->current_frame++;', 1)]:
    try:
        patch.apply_frame_retry(invalid)
    except SystemExit:
        pass
    else:
        raise AssertionError('changed/partial callback must be refused')
use_fixture = args.fixture_only or not vendor_path.is_file()
vendor = patched_fixture if use_fixture else vendor_path.read_text()
if not args.skip_patch_conformance:
    assert patch.apply_frame_retry(vendor) == vendor, 'generated callback must match tracked patch'
    for signature in signatures:
        assert function(vendor, signature) == function(patched_fixture, signature), \
            'managed playback drifted from pinned fixture: ' + signature
functions = '\n\n'.join(function(vendor, signature) for signature in signatures)
tick = function(display, 'static bool presenter_quiet_tick(jr_display_ctx_t *ctx, uint32_t *last_word)')
a = tick.index('        esp_err_t err = gfx_anim_stop')
b = tick.index('        (void)gfx_emote_unlock', a)
sequence = tick[a:b]
digest = hashlib.sha256((functions + sequence).encode()).hexdigest()
scratch = (root / '.evidence/t107/quiet-display/vendor' /
           (digest + ('-fixture' if use_fixture else '-managed'))).resolve()
assert scratch.is_relative_to(root / '.evidence/t107')
scratch.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define CHECK_OBJ_TYPE_ANIMATION(o) do { if (!(o)) return ESP_ERR_INVALID_STATE; } while (0)
#define GFX_DISP_EVENT_ALL_FRAME_DONE 1
#define GFX_DISP_EVENT_ONE_FRAME_DONE 2
typedef struct gfx_obj gfx_obj_t;
typedef struct gfx_disp gfx_disp_t;
struct gfx_disp { void (*update_cb)(gfx_disp_t *, int, gfx_obj_t *); };
typedef struct { void *file_desc, *timer; uint32_t start_frame, end_frame, current_frame, fps;
                 bool repeat, is_playing; } gfx_anim_t;
struct gfx_obj { void *src; gfx_disp_t *disp; struct { bool is_visible; } state; };
typedef struct { gfx_obj_t *anim; int shown_face; } jr_display_ctx_t;
static unsigned prepares, invalidations, prepared_frame, period_ms, done_events, failed_prepares, frame_events;
static int eaf_get_total_frames(void *file) { (void)file; return 8; }
static void gfx_timer_set_period(void *timer, unsigned period) { (void)timer; period_ms = period; }
static esp_err_t gfx_anim_prepare_frame(gfx_obj_t *obj)
{ prepares++; prepared_frame = ((gfx_anim_t *)obj->src)->current_frame;
  if (failed_prepares) { failed_prepares--; return ESP_ERR_INVALID_STATE; } return ESP_OK; }
static void gfx_obj_invalidate(gfx_obj_t *obj) { (void)obj; invalidations++; }
static esp_err_t gfx_obj_set_visible(gfx_obj_t *obj, bool visible)
{ obj->state.is_visible = visible; return ESP_OK; }
static unsigned face_fps(int face) { (void)face; return 8; }
static void update(gfx_disp_t *disp, int event, gfx_obj_t *obj)
{ (void)disp; (void)obj; if (event == GFX_DISP_EVENT_ALL_FRAME_DONE) done_events++; else frame_events++; }
'''
harness += functions
harness += '\nstatic esp_err_t production_quiet_begin(jr_display_ctx_t *ctx) {\n' + sequence + '\nreturn err; }\n'
harness += r'''
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line%d: %s\n", __LINE__, #x); } } while (0)
int main(void)
{
 gfx_anim_t anim = {.file_desc=(void *)1,.timer=(void *)2,.fps=24,.is_playing=true,
                    .current_frame=5,.start_frame=0,.end_frame=7,.repeat=true};
 gfx_disp_t disp = {.update_cb=update};
 gfx_obj_t obj = {.src=&anim,.disp=&disp,.state={true}};
 jr_display_ctx_t ctx = {.anim=&obj};
 CHECK(production_quiet_begin(&ctx) == ESP_OK);
 CHECK(anim.is_playing && !anim.repeat && anim.start_frame == 0 && anim.end_frame == 1);
 CHECK(obj.state.is_visible && period_ms == 125);
 failed_prepares = 2; /* transient allocation/parser failure before the first frame */
 gfx_anim_timer_callback(&obj);
 CHECK(prepares == 1 && anim.is_playing && anim.current_frame == 0 && !invalidations);
 CHECK(frame_events == 0 && done_events == 0);
 gfx_anim_timer_callback(&obj);
 CHECK(prepares == 2 && anim.is_playing && anim.current_frame == 0 && !invalidations);
 CHECK(frame_events == 0 && done_events == 0);
 gfx_anim_timer_callback(&obj);
 CHECK(prepares == 3 && prepared_frame == 0 && invalidations == 1 && anim.current_frame == 1);
 CHECK(frame_events == 1);
 gfx_anim_timer_callback(&obj);
 CHECK(!anim.is_playing && done_events == 1 && obj.state.is_visible && anim.file_desc == (void *)1);
 for (unsigned i=0; i<720*8; ++i) {
   gfx_anim_timer_callback(&obj);
   CHECK(prepares == 3 && invalidations == 1 && obj.state.is_visible);
 }
 gfx_obj_invalidate(&obj); /* explicit facts/snapshot refresh retains parsed frame0 */
 CHECK(prepares == 3 && prepared_frame == 0 && !anim.is_playing && invalidations == 2);
 CHECK(gfx_anim_set_segment(&obj,0,7,8,true) == ESP_OK && gfx_anim_start(&obj) == ESP_OK);
 for (unsigned i=0; i<16; ++i) gfx_anim_timer_callback(&obj);
 CHECK(anim.is_playing && prepares > 1 && invalidations == 18 && done_events == 3);
 CHECK(gfx_anim_stop(&obj) == ESP_OK && gfx_anim_set_segment(&obj,0,0,8,false) == ESP_OK);
 CHECK(gfx_anim_start(&obj) == ESP_OK);
 unsigned before = prepares;
 gfx_anim_timer_callback(&obj);
 CHECK(!anim.is_playing && prepares == before); /* [0,0] never prepares a frame */
 anim.file_desc = NULL;
 CHECK(gfx_anim_start(&obj) == ESP_ERR_INVALID_STATE);
 obj.src = NULL;
 CHECK(gfx_anim_start(&obj) == ESP_ERR_INVALID_STATE && gfx_anim_stop(&obj) == ESP_ERR_INVALID_STATE);
 printf("vendor quiet tests: %u checks, %u failures\n",checks,failures);
 if (!failures && checks) printf("vendor quiet passed (%u checks)\n", checks);
 return failures ? 1 : 0;
}
'''
(scratch / 'test.c').write_text(harness)
command = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
           str(scratch / 'test.c'), '-o', str(scratch / 'test')]
compiled = subprocess.run(command, capture_output=True, text=True)
(scratch / 'compile.log').write_text(compiled.stdout + compiled.stderr)
if compiled.returncode: raise SystemExit(compiled.stderr)
run = subprocess.run([str(scratch / 'test')], capture_output=True, text=True)
(scratch / 'run.log').write_text(run.stdout + run.stderr)
(scratch / 'receipt.json').write_text(json.dumps(dict(
    display_sha256=hashlib.sha256(display_path.read_bytes()).hexdigest(),
    vendor_sha256=hashlib.sha256(vendor.encode()).hexdigest(),
    vendor_input='patched licensed fixture' if use_fixture else str(vendor_path.relative_to(root)),
    fixture_sha256=hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
    extracted_sha256=digest, compile_rc=compiled.returncode, run_rc=run.returncode), indent=2)+'\n')
print(run.stdout, end='')
print('artifact:', scratch.relative_to(root))
raise SystemExit(run.returncode)
