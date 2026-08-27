#!/usr/bin/env python3
"""Apply required, idempotent fixes to v5 managed ESP-IDF components."""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WS_CLIENT = (
    ROOT
    / "managed_components"
    / "espressif__esp_websocket_client"
    / "esp_websocket_client.c"
)
BACKPRESSURE_MARKER = "JarvisNano v5: transient TCP would-block"
BACKPRESSURE_ANCHOR = "        if (wlen < 0 || (wlen == 0 && need_write != 0)) {\n"
BACKPRESSURE_INSERT = f"""        /* {BACKPRESSURE_MARKER} at frame start is not fatal. Nothing from this
         * frame reached the wire, so return backpressure without aborting the
         * live socket. A real error or mid-frame stall remains fatal below. */
        if (wlen == 0 && widx == 0 && need_write != 0) {{
            ret = 0;
            esp_websocket_free_buf(client, true);
            goto unlock_and_return;
}}
"""

TASK_MARKER = "JarvisNano v5: PSRAM websocket task stack"
TASK_INCLUDE_ANCHOR = '#include "freertos/task.h"\n'
TASK_INCLUDE_INSERT = '''#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
'''
TASK_DELETE_ANCHOR = "    vTaskDelete(NULL);\n}\n\nesp_err_t esp_websocket_client_start"
TASK_DELETE_INSERT = """#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \\
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && \\
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \\
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \\
    CONFIG_SPIRAM && defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && \\
    CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

esp_err_t esp_websocket_client_start"""
TASK_CREATE_ANCHOR = """    if (xTaskCreatePinnedToCore(esp_websocket_client_task, client->config->task_name ? client->config->task_name : "websocket_task",
                                client->config->task_stack, client, client->config->task_prio, &client->task_handle, client->config->task_core_id) != pdTRUE) {
        ESP_LOGE(TAG, "Error create websocket task");
        return ESP_FAIL;
    }
"""
TASK_CREATE_INSERT = f"""    /* {TASK_MARKER}. This worker is never used by an ISR or DMA and
     * otherwise competes with the internal-only voice owner, codec DMA, and
     * hardware AES for one late contiguous allocation. */
    BaseType_t task_created;
#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \\
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && \\
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \\
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \\
    CONFIG_SPIRAM && defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && \\
    CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    task_created = xTaskCreatePinnedToCoreWithCaps(
        esp_websocket_client_task,
        client->config->task_name ? client->config->task_name : "websocket_task",
        client->config->task_stack, client, client->config->task_prio,
        &client->task_handle, client->config->task_core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    task_created = xTaskCreatePinnedToCore(
        esp_websocket_client_task,
        client->config->task_name ? client->config->task_name : "websocket_task",
        client->config->task_stack, client, client->config->task_prio,
        &client->task_handle, client->config->task_core_id);
#endif
    if (task_created != pdTRUE) {{
        ESP_LOGE(TAG, "Error create websocket task");
        return ESP_FAIL;
    }}
"""


GFX_ANIM = (
    ROOT
    / "managed_components"
    / "espressif2022__esp_emote_gfx"
    / "src"
    / "widget"
    / "gfx_anim.c"
)
PALETTE_MARKER = "JarvisNano v5: palette PSRAM fallback"
PALETTE_ANCHOR = (
    "        anim->frame.color_palette = (uint32_t *)heap_caps_malloc("
    "palette_size * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);\n"
)
PALETTE_INSERT = f"""        anim->frame.color_palette = (uint32_t *)heap_caps_malloc(palette_size * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        /* {PALETTE_MARKER}. Internal RAM is the scarce pool on this build
         * (WakeNet scratch joined the budget); a <=1 KB palette reads fine
         * through the cache from PSRAM, while a failed alloc here drops every
         * subsequent frame of the face. */
        if (anim->frame.color_palette == NULL) {{
            anim->frame.color_palette = (uint32_t *)heap_caps_malloc(palette_size * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the managed source already contains every required patch",
    )
    args = parser.parse_args()

    if not WS_CLIENT.is_file():
        raise SystemExit(
            "managed esp_websocket_client is missing; run IDF dependency "
            "resolution before patch-v5-managed.py"
        )

    source = WS_CLIENT.read_text()
    changed: list[str] = []
    missing: list[str] = []

    if BACKPRESSURE_MARKER not in source:
        if args.check:
            missing.append("would-block fix")
        else:
            if source.count(BACKPRESSURE_ANCHOR) != 1:
                raise SystemExit(
                    "esp_websocket_client send-error anchor changed; refusing a blind patch"
                )
            source = source.replace(
                BACKPRESSURE_ANCHOR,
                BACKPRESSURE_INSERT + BACKPRESSURE_ANCHOR,
                1,
            )
            changed.append("would-block fix")

    task_fragments = (
        TASK_MARKER,
        '#include "freertos/idf_additions.h"',
        "vTaskDeleteWithCaps(NULL);",
    )
    task_present = [fragment in source for fragment in task_fragments]
    if any(task_present) and not all(task_present):
        raise SystemExit(
            "esp_websocket_client PSRAM-task patch is partial; refusing a blind repair"
        )
    if not all(task_present):
        if args.check:
            missing.append("PSRAM websocket task stack")
        else:
            anchors = (
                (TASK_INCLUDE_ANCHOR, TASK_INCLUDE_INSERT),
                (TASK_DELETE_ANCHOR, TASK_DELETE_INSERT),
                (TASK_CREATE_ANCHOR, TASK_CREATE_INSERT),
            )
            for anchor, insert in anchors:
                if source.count(anchor) != 1:
                    raise SystemExit(
                        "esp_websocket_client task anchor changed; refusing a blind patch"
                    )
                source = source.replace(anchor, insert, 1)
            changed.append("PSRAM websocket task stack")

    ws_changed = bool(changed)

    if GFX_ANIM.is_file():
        gfx_source = GFX_ANIM.read_text()
        if PALETTE_MARKER not in gfx_source:
            if args.check:
                missing.append("gfx palette PSRAM fallback")
            else:
                if gfx_source.count(PALETTE_ANCHOR) != 1:
                    raise SystemExit(
                        "gfx_anim palette anchor changed; refusing a blind patch"
                    )
                gfx_source = gfx_source.replace(PALETTE_ANCHOR, PALETTE_INSERT, 1)
                GFX_ANIM.write_text(gfx_source)
                changed.append("gfx palette PSRAM fallback")
    elif args.check:
        missing.append("gfx palette PSRAM fallback (gfx_anim.c missing)")

    if missing:
        raise SystemExit("v5 managed patch missing: " + ", ".join(missing))
    if changed:
        if ws_changed:
            WS_CLIENT.write_text(source)
        print("v5 managed patch: applied " + ", ".join(changed))
    else:
        print("v5 managed patch: all fixes present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
