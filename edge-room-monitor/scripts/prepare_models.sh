#!/usr/bin/env bash
set -euo pipefail

APP_ROOT=${APP_ROOT:-/workspace/edge-room-monitor}
MODEL_ROOT=${MODEL_ROOT:-$APP_ROOT/models}
YOLO_NAME=${YOLO_NAME:-yolov8n}
YOLO_DIR="$MODEL_ROOT/$YOLO_NAME"
ONNX_PATH="$YOLO_DIR/${YOLO_NAME}.onnx"
ENGINE_PATH="$YOLO_DIR/${YOLO_NAME}_fp16.engine"
TRTEXEC=${TRTEXEC:-/usr/src/tensorrt/bin/trtexec}
ONNX_URL=${ONNX_URL:-}
URL_HINTS=(
  "$YOLO_DIR/${YOLO_NAME}.onnx.url"
  "$MODEL_ROOT/${YOLO_NAME}.onnx.url"
)

log() { printf '[models] %s\n' "$*"; }
err() { printf '[models ERROR] %s\n' "$*" >&2; }

mkdir -p "$YOLO_DIR"

resolve_download_url() {
  if [[ -n "${ONNX_URL:-}" ]]; then
    return
  fi

  for candidate in "${URL_HINTS[@]}"; do
    if [[ -f "$candidate" ]]; then
      local value
      value=$(grep -v '^\s*#' "$candidate" | head -n1 | tr -d '\r')
      if [[ -n "$value" ]]; then
        ONNX_URL="$value"
        log "ONNX URL を $candidate から読み込みました"
        return
      fi
    fi
  done
}

download_onnx() {
  if [[ -f "$ONNX_PATH" ]]; then
    log "既存の ONNX を使用します: $ONNX_PATH"
    return
  fi
  resolve_download_url
  if [[ -z "${ONNX_URL:-}" ]]; then
    err "ONNX が存在せずダウンロードURLも設定されていません。${URL_HINTS[*]} のいずれかに URL を記述するか、$ONNX_PATH にファイルを配置してください。"
    exit 1
  fi
  log "YOLOv8n ONNX を取得: $ONNX_URL"
  tmp=$(mktemp -p "$YOLO_DIR" "${YOLO_NAME}.onnx.XXXXXX")
  if command -v wget >/dev/null 2>&1; then
    if ! wget -q -O "$tmp" "$ONNX_URL"; then
      rm -f "$tmp"
      err "ONNX のダウンロードに失敗しました (wget)"
      exit 1
    fi
  elif command -v curl >/dev/null 2>&1; then
    if ! curl -fsSL "$ONNX_URL" -o "$tmp"; then
      rm -f "$tmp"
      err "ONNX のダウンロードに失敗しました (curl)"
      exit 1
    fi
  else
    rm -f "$tmp"
    err "wget / curl が利用できません"
    exit 1
  fi
  mv "$tmp" "$ONNX_PATH"
}

build_engine() {
  if [[ -f "$ENGINE_PATH" ]]; then
    return
  fi
  if [[ ! -x "$TRTEXEC" ]]; then
    err "trtexec が見つかりません ($TRTEXEC)"
    exit 1
  fi
  log "TensorRT エンジンを生成 ($ENGINE_PATH)"
  "$TRTEXEC" \
    --onnx="$ONNX_PATH" \
    --saveEngine="$ENGINE_PATH" \
    --workspace=2048 \
    --fp16 \
    >/tmp/trtexec.log 2>&1 || {
      cat /tmp/trtexec.log >&2
      err "TensorRT エンジン生成に失敗しました"
      exit 1
    }
}

download_onnx
build_engine
