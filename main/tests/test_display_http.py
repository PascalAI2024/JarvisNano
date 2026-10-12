#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual display serializer; validate worst-width transfer metadata."""
import argparse, json, shutil, subprocess, tempfile
from pathlib import Path
from test_companion_main import function

MUTANTS = {
    'optical-forged': ('\\"optical\\":false', '\\"optical\\":true'),
    'glance-valid-forged': ('d.boot_timing.first_glance_time_transfer_valid ? "true" : "false"', '"true"'),
    'glance-time-swapped': ('(unsigned)d.boot_timing.first_glance_time_transfer_done_boot_ms', '(unsigned)d.boot_timing.first_transfer_done_boot_ms'),
    'rom-forged': ('\\\"includes_rom\\\":false', '\\\"includes_rom\\\":true'),
}

def run(root, out, mutation):
    handler=function((root/'main/http_routes.c').read_text(encoding='utf-8'),
                     'static esp_err_t display_diag_handler(')
    if mutation:
        before,after=MUTANTS[mutation];assert handler.count(before)==1
        handler=handler.replace(before,after)
    out.mkdir(parents=True,exist_ok=True)
    source=out/'display.c';binary=out/'test_display_http'
    source.write_text((root/'main/tests/test_display_http.c.in').read_text().replace('__PRODUCTION_HANDLER__',handler))
    command=[shutil.which('cc') or shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror',str(source)]
    for include in ('components/jr_display/include','components/jr_ports/include','components/jr_display/tests/probe/stubs'):
        command+=['-I',str(root/include)]
    subprocess.run(command+['-o',str(binary)],check=True)
    checks=0
    def equal(actual,expected):
        nonlocal checks
        checks+=1
        assert type(actual) is type(expected) and actual==expected,(actual,expected)
    for bits in range(16):
        p=subprocess.run([str(binary),str(bits)],check=True,capture_output=True,text=True)
        (out/(str(bits)+'.txt')).write_text(p.stdout)
        meta,body=p.stdout.split('\n',1);meta=json.loads(meta)
        equal(meta['result'],0)
        equal(meta['errors'],1 if bits&4 else 0)
        equal(meta['sent'],0 if bits&4 else 1)
        if bits&4: continue
        equal(meta['type'],'application/json');equal(meta['cache'],'no-store')
        equal(meta['bytes']<1024,True)
        doc=json.loads(body);timing=doc['boot_timing']
        equal(doc['init'],'failed' if bits&8 else 'ready')
        equal(doc['last_error'],'ESP_FAIL' if bits&8 else 'ESP_OK')
        equal(doc['requests'],4294967295)
        equal(timing['time_base'],'esp_timer_since_boot')
        equal(timing['optical'],False);equal(timing['includes_rom'],False)
        equal(timing['first_transfer_valid'],bool(bits&1))
        equal(timing['first_transfer_done_boot_ms'],4294967295 if bits&1 else 0)
        equal(timing['first_glance_time_transfer_valid'],bool(bits&2))
        equal(timing['first_glance_time_transfer_done_boot_ms'],987654321 if bits&2 else 0)
        equal(timing['init_start_boot_ms'],4294967293)
        equal(timing['init_end_boot_ms'],4294967294)
    print(f'display HTTP passed ({checks} checks)')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mutation',choices=MUTANTS)
    p.add_argument('--output-dir',type=Path);a=p.parse_args();root=Path(__file__).resolve().parents[2]
    if a.output_dir: run(root,a.output_dir,a.mutation)
    else:
        with tempfile.TemporaryDirectory(prefix='jr-display-http-') as d: run(root,Path(d),a.mutation)
