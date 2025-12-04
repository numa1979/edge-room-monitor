#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${APP_ROOT:-}" ]]; then
  if [[ -d /workspace/edge-room-monitor ]]; then
    APP_ROOT=/workspace/edge-room-monitor
  else
    SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    APP_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
  fi
fi
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
  apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    wget \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-libav \
    gstreamer1.0-tools
  mkdir -p "$(dirname "$MARKER_FILE")"
  touch "$MARKER_FILE"
fi

MODEL_PREP_SCRIPT="$APP_ROOT/scripts/prepare_models.sh"
if [ -x "$MODEL_PREP_SCRIPT" ]; then
  "$MODEL_PREP_SCRIPT"
fi

if [[ $# -gt 0 ]]; then
  info "カスタムコマンドを実行: $*"
  exec bash -lc "$*"
fi

YOLO_PARSER_SCRIPT="$APP_ROOT/scripts/build_yolo_parser.sh"
if [ -x "$YOLO_PARSER_SCRIPT" ]; then
  "$YOLO_PARSER_SCRIPT"
fi

info "CMake configure"
mkdir -p "$BUILD_DIR"
(cd "$BUILD_DIR" && cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "$APP_ROOT")
info "CMake build"
cmake --build "$BUILD_DIR"

APP_BIN="$BUILD_DIR/edge-room-monitor"
# デフォルトは推論パイプライン（YOLOv8検出あり）
PIPELINE_CONFIG=${PIPELINE_CONFIG:-$APP_ROOT/configs/camera_infer.pipeline}
if [ ! -x "$APP_BIN" ]; then
  err "ビルド成果物 ${APP_BIN} がありません"
  exit 1
fi

info "HTTP ポート ${APP_HTTP_PORT} で MJPEG プレビューを公開"
info "パイプライン: ${PIPELINE_CONFIG}"
exec env \
  GST_DEBUG=3 \
  GST_DEBUG_NO_COLOR=1 \
  APP_HTTP_PORT="${APP_HTTP_PORT}" \
  PIPELINE_CONFIG="${PIPELINE_CONFIG}" \
  APP_CAMERA_DEVICE="${APP_CAMERA_DEVICE:-}" \
  "$APP_BIN" 2>&1 | tee /tmp/app.log
