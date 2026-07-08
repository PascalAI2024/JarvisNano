/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/power_monitor.h — PowerMonitor port (L0 AXP2101 side).
 *
 * Read-only battery/charge telemetry. The AXP2101 uses init_skip (hardware.md
 * non-negotiable #10) — this port reads rails, it never reprograms them.
 * Feeds the HUD battery arc (Phase 4) and low-power policy.
 */
#ifndef JR_PORTS_POWER_MONITOR_H
#define JR_PORTS_POWER_MONITOR_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jr_power_monitor {
    void *ctx;
    int  (*battery_pct)(void *ctx);   /* 0..100, or negative jr_err_t */
    bool (*is_charging)(void *ctx);
} jr_power_monitor_t;

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_POWER_MONITOR_H */
