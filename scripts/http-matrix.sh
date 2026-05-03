#!/usr/bin/env bash
set -euo pipefail

TIMEOUT="${CURL_TIMEOUT:-4}"
ENDPOINTS=(
    "/api/health"
    "/api/status"
    "/api/battery"
    "/api/audio/level"
    "/api/wifi/scan"
)

if [ "$#" -gt 0 ]; then
    HOSTS=("$@")
else
    HOSTS=(
        "192.168.4.1"
        "192.168.50.80"
        "esp-claw.local"
    )
fi

printf 'HTTP matrix timeout=%ss\n' "$TIMEOUT"

for host in "${HOSTS[@]}"; do
    base="$host"
    base="${base#http://}"
    base="${base#https://}"
    base="${base%/}"
    printf '\n== %s ==\n' "$base"

    for endpoint in "${ENDPOINTS[@]}"; do
        url="http://$base$endpoint"
        output="$(
            curl -m "$TIMEOUT" -sS -o /dev/null \
                -w '\n%{http_code} %{size_download} %{time_total}' \
                "$url" 2>&1
        )" || {
            metrics="$(printf '%s\n' "$output" | tail -n 1)"
            error="$(printf '%s\n' "$output" | sed '$d' | tr '\n' ' ')"
            read -r code bytes seconds <<< "$metrics"
            printf 'FAIL %-20s http=%s bytes=%s time=%ss %s\n' "$endpoint" "$code" "$bytes" "$seconds" "$error"
            continue
        }
        metrics="$(printf '%s\n' "$output" | tail -n 1)"
        read -r code bytes seconds <<< "$metrics"
        if [[ "$code" == 2* ]]; then
            label="OK"
        else
            label="WARN"
        fi
        printf '%-4s %-20s http=%s bytes=%s time=%ss\n' "$label" "$endpoint" "$code" "$bytes" "$seconds"
    done
done
