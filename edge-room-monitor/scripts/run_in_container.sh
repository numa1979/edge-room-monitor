#!/usr/bin/env bash
set -euo pipefail

APP_ROOT=/workspace/edge-room-monitor
BUILD_DIR=${BUILD_DIR:-$APP_ROOT/build}
MARKER_FILE=${MARKER_FILE:-/var/local/.edge-room-monitor-deps}
APP_HTTP_PORT=${APP_HTTP_PORT:-8080}

info() { printf '[container] %s\n' "$*"; }
warn() { printf '[container WARN] %s\n' "$*" >&2; }
err()  { printf '[container ERROR] %s\n' "$*" >&2; }

if [ ! -f "$MARKER_FILE" ]; then
  info "初回セットアップ: 開発ツールを導入"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y build-essential cmake ninja-build git wget pkg-config
  mkdir -p "$(dirname "$MARKER_FILE")"
  touch "$MARKER_FILE"
fi

info "CMake configure"
mkdir -p "$BUILD_DIR"
(cd "$BUILD_DIR" && cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "$APP_ROOT")
info "CMake build"
cmake --build "$BUILD_DIR"

APP_BIN="$BUILD_DIR/edge-room-monitor"
if [ ! -x "$APP_BIN" ]; then
  err "ビルド成果物 ${APP_BIN} がありません"
  exit 1
fi

info "HTTP ポート ${APP_HTTP_PORT} (未実装 placeholder)"
exec "$APP_BIN"
