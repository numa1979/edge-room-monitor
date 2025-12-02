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
run_step "Dockerコンテナ/アプリ" "$MODULES_DIR/setup_docker_env.sh"

echo "[prod] 完了: Jetson が起動すると FastAPI アプリが http://<IP>:8080 で応答します"
