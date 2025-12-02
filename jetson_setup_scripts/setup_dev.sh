#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MODULES_DIR="$SCRIPT_DIR/modules"

run_step() {
  local step_name="$1"
  shift
  echo "[dev] >>> ${step_name}"
  "$@"
}

run_step "ホストパッケージ" "$MODULES_DIR/install_host_packages.sh"
run_step "Wi-Fiセットアップ" "$MODULES_DIR/setup_wifi.sh"
run_step "日本語入力" "$MODULES_DIR/setup_japanese_input.sh"
run_step "Dockerコンテナ/アプリ" "$MODULES_DIR/setup_docker_env.sh"
run_step "Remote SSH" "$MODULES_DIR/setup_remote_ssh.sh"

echo "[dev] 完了: VS Code で 22.04 コンテナに接続して開発できます"
