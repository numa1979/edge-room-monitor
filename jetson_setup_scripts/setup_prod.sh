#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MODULES_DIR="$SCRIPT_DIR/modules"

run_step() {
  local step_name="$1"
  shift
  echo "[prod] >>> ${step_name}"
  "$@"
}

run_step "ホストパッケージ" "$MODULES_DIR/install_host_packages.sh"
run_step "Wi-Fiセットアップ" "$MODULES_DIR/setup_wifi.sh"
run_step "アプリ自動起動設定" "$MODULES_DIR/setup_app_service.sh"

get_ipv4_cidr() {
  local dev="$1"
  ip -4 addr show "$dev" 2>/dev/null | awk '/inet / {print $2}' | head -n1
}

HTTP_PORT=${APP_HTTP_PORT:-8080}
endpoints=()
add_endpoint() {
  local dev="$1" label="$2"
  if ip_cidr=$(get_ipv4_cidr "$dev"); then
    if [ -n "$ip_cidr" ]; then
      local ip="${ip_cidr%/*}"
      endpoints+=("${label}(${dev}): http://${ip}:${HTTP_PORT}")
    fi
  fi
}

add_endpoint wlan0 "Wi-Fi"
add_endpoint eth0 "有線"

if [ ${#endpoints[@]} -gt 0 ]; then
  endpoint_list=$(IFS=', '; echo "${endpoints[*]}")
  echo "[prod] 完了: ${endpoint_list} で応答します"
else
  primary_ip=$(hostname -I | awk '{print $1}')
  if [ -n "$primary_ip" ]; then
    echo "[prod] 完了: http://${primary_ip}:${HTTP_PORT} で応答します"
  else
    echo "[prod] 完了: IP は取得できませんでした"
  fi
fi
