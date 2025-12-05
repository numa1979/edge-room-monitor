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

endpoints=()
if ip_cidr=$(get_ipv4_cidr wlan0); then
  if [ -n "$ip_cidr" ]; then
    endpoints+=("Wi-Fi(wlan0): http://${ip_cidr%/*}:8080")
  fi
fi
if ip_cidr=$(get_ipv4_cidr eth0); then
  if [ -n "$ip_cidr" ]; then
    endpoints+=("有線(eth0): http://${ip_cidr%/*}:8080")
  fi
fi

if [ ${#endpoints[@]} -gt 0 ]; then
  endpoint_list=$(IFS=', '; echo "${endpoints[*]}")
  echo "[prod] 完了: Jetson が起動すると edge-room-monitor アプリが ${endpoint_list} で応答します"
else
  primary_ip=$(hostname -I | awk '{print $1}')
  if [ -n "$primary_ip" ]; then
    echo "[prod] 完了: Jetson が起動すると edge-room-monitor アプリが http://${primary_ip}:8080 で応答します"
  else
    echo "[prod] 完了: Jetson が起動すると edge-room-monitor アプリが http://<IP>:8080 で応答します (IP は取得できませんでした)"
  fi
fi
