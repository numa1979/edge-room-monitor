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

echo "[prod] 完了: Jetson が起動すると edge-room-monitor アプリが http://<IP>:8080 で応答します"
