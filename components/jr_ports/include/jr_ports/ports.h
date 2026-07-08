/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/ports.h — umbrella include for every port interface.
 *
 * The composition root (L6, main/main.c) includes this to obtain concrete
 * adapters from L0/L1/L2/L5 and inject them into the core. This header, and
 * everything it pulls in, is pure C with NO ESP-IDF dependency — that is the
 * invariant the host build enforces.
 */
#ifndef JR_PORTS_PORTS_H
#define JR_PORTS_PORTS_H

#include "jr_ports/jr_types.h"
#include "jr_ports/clock.h"
#include "jr_ports/audio_source.h"
#include "jr_ports/audio_sink.h"
#include "jr_ports/realtime_voice_client.h"
#include "jr_ports/display.h"
#include "jr_ports/input.h"
#include "jr_ports/power_monitor.h"
#include "jr_ports/nvs.h"

#endif /* JR_PORTS_PORTS_H */
