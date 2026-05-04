#!/usr/bin/env bash
set -euo pipefail

TIMEOUT="${CURL_TIMEOUT:-4}"
ENDPOINTS=(
    "/api/health"
    "/api/status"
    "/api/config"
    "/api/webim/status"
    "/api/battery"
    "/api/audio/level"
    "/api/wifi/scan"
    "/api/camera/snapshot"
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

    options_url="http://$base/api/status"
    if output="$(
        curl -m "$TIMEOUT" -sS -o /dev/null \
            -X OPTIONS \
            -H 'Access-Control-Request-Method: GET' \
            -H 'Origin: http://localhost' \
            -w '\n%{http_code} %{size_download} %{time_total}' \
            "$options_url" 2>&1
    )"; then
        metrics="$(printf '%s\n' "$output" | tail -n 1)"
        read -r code bytes seconds <<< "$metrics"
        if [[ "$code" == 2* || "$code" == 204 ]]; then label="OK"; else label="WARN"; fi
        printf '%-4s %-20s http=%s bytes=%s time=%ss\n' "$label" "OPTIONS /api/status" "$code" "$bytes" "$seconds"
    else
        metrics="$(printf '%s\n' "$output" | tail -n 1)"
        error="$(printf '%s\n' "$output" | sed '$d' | tr '\n' ' ')"
        read -r code bytes seconds <<< "$metrics"
        printf 'FAIL %-20s http=%s bytes=%s time=%ss %s\n' "OPTIONS /api/status" "$code" "$bytes" "$seconds" "$error"
    fi

    ws_url="http://$base/ws/webim"
    if output="$(
        curl -m "$TIMEOUT" -sS -o /dev/null \
            -H 'Connection: Upgrade' \
            -H 'Upgrade: websocket' \
            -H 'Sec-WebSocket-Version: 13' \
            -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
            -w '\n%{http_code} %{size_download} %{time_total}' \
            "$ws_url" 2>&1
    )"; then
        metrics="$(printf '%s\n' "$output" | tail -n 1)"
        read -r code bytes seconds <<< "$metrics"
        if [[ "$code" == 101 ]]; then label="OK"; else label="WARN"; fi
        printf '%-4s %-20s http=%s bytes=%s time=%ss\n' "$label" "WS /ws/webim" "$code" "$bytes" "$seconds"
    else
        metrics="$(printf '%s\n' "$output" | tail -n 1)"
        error="$(printf '%s\n' "$output" | sed '$d' | tr '\n' ' ')"
        read -r code bytes seconds <<< "$metrics"
        printf 'FAIL %-20s http=%s bytes=%s time=%ss %s\n' "WS /ws/webim" "$code" "$bytes" "$seconds" "$error"
    fi
done
