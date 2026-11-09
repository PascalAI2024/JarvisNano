# NATURAL-HARNESS final review

Result: ACCEPTED for the coordinator's unattended natural-ladder/default-timer run. This is offline harness acceptance, not a device result.

Reviewed harness: `.evidence/t107d/natural_run.py`

Exact file-byte SHA-256: `6481b13bc32272258ff8f11c1caf9d049e3cc0270ef655ac7708739a79fb2290`

The reviewed bytes were unchanged before and after the simulation. PowerShell `Get-FileHash -Algorithm SHA256` independently returned the same digest.

## Evidence

- Line 17 now requires `health.voice_alive`, including the first observation after timer wake. The actual authenticated health handler publishes this field at `main/http_routes.c:544`; its heartbeat definition is at lines 481-482. Privacy takes precedence over session-dead in the health verdict at lines 515-523, so the independent liveness assertion is necessary.
- Line 22 now requires IMU age strictly below 500 ms, matching the production orientation input at `main/main.c:2332-2335`. A 499 ms fixture passes; 500 ms and 1000 ms fixtures fail.
- The explicit stage order and transition brackets remain intact. Required observed order is AWAKE, WHISPER, DREAM; the muted path does not claim an AMBIENT observation. Transition and API-loss brackets must intersect the configured natural thresholds and remain at most 45 seconds wide.
- Static call review confirms `run.lib.home()` is the final control action: the helper closes the shade and posts the privacy-safe synthetic HOME double. The subsequent natural path calls only the five fixed GET endpoints. Importing `device_run.py` does not execute its OTA/idle CLI because its dispatch is guarded by `__name__ == '__main__'`.
- Both scripts passed Python `ast.parse`.
- An in-memory simulation executed the actual reviewed `natural_run.py` functions with a fake `device_run` module, clock and GET responses. The real device helper was never imported. Receipt writers were replaced with in-memory collections. The simulation made no network calls or file writes, and asserted that all calls after the two initial HOME helper POSTs were GETs.

The offline Python check completed with exit code 0 (`exec_command` output chunk `f93628`):

| Fixture | Result |
| --- | --- |
| Normal natural ladder and 14400 s timer | PASS |
| IMU age 499 ms | PASS |
| Premature DREAM | REJECT: stage timing |
| Premature sleep | REJECT: required stages missing |
| Unexpected AMBIENT | REJECT: stage order |
| Face-down during DREAM | REJECT: orientation |
| 3600 s timer | REJECT: default 4h timing |
| Lift wake | REJECT: non-timer wake |
| RTC sleep counter increases twice | REJECT: counter |
| Privacy lost on wake | REJECT: privacy |
| Voice task dead initially | REJECT: voice task is not alive |
| Voice task dead on timer wake | REJECT: voice task is not alive |
| IMU age 500 ms initially | REJECT: freshness |
| IMU age 500 ms on timer wake | REJECT: freshness |
| IMU age 1000 ms | REJECT: freshness |

Total: 15 fixtures; two positive fixtures accepted and thirteen adverse fixtures rejected. No new actionable defect found in the two final gate changes.

## Changed artifacts

Only this review file. No production source, harness, device, full gate or commit was changed or operated by this review.

## Remaining risk

API disappearance is a bounded observation interval, not an electrical sleep edge. The eventual timer wake cause, exactly one RTC sleep increment and host elapsed interval support the default timer inference. Five-minute offline probes cannot continuously exclude every network-only outage between probes. Stage thresholds are accepted within the recorded observation brackets, not as exact electrical timestamps. The current API does not expose `esp_reset_reason`; the harness correctly reports reset reason as unavailable. Offline fixtures do not prove physical orientation, battery behavior, actual deep sleep, wake latency or owner voice behavior.

## Decision needed

None for this bounded review. The coordinator may execute the reviewed harness under the existing muted/no-lease protocol and must preserve its live receipts before claiming natural-ladder/default-timer acceptance.
