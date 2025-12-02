#!/usr/bin/env bash
# Launch the DeepStream container, build the app, and start streaming/logging.
set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CONTAINER_NAME=${CONTAINER_NAME:-edge-room-monitor-app}
IMAGE_NAME=${IMAGE_NAME:-nvcr.io/nvidia/deepstream-l4t:6.0.1-samples}
CONTAINER_RUNTIME=${CONTAINER_RUNTIME:-nvidia}
PRIVILEGED_MODE=${PRIVILEGED_MODE:-true}
APP_HTTP_PORT=${APP_HTTP_PORT:-8080}
DOCKER_NETWORK=${DOCKER_NETWORK:-host}

info() { printf '\033[1;34m[start-app]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[start-app WARN]\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31m[start-app ERROR]\033[0m %s\n' "$*" >&2; }

command -v docker >/dev/null 2>&1 || { err "docker コマンドが見つかりません"; exit 1; }
DOCKER=(docker)
if ! "${DOCKER[@]}" info >/dev/null 2>&1; then
  if sudo -n docker info >/dev/null 2>&1; then
    info "docker へのアクセスに sudo を使用します"
    DOCKER=(sudo docker)
  else
    err "docker 情報を取得できません。docker グループや sudo 設定を確認してください"
    exit 1
  fi
fi

if ! "${DOCKER[@]}" info --format '{{json .Runtimes}}' | grep -q 'nvidia'; then
  warn "nvidia-container-runtime が見つかりません。Jetson で GPU が使えない可能性があります"
fi

DEVICE_ARGS=()
for dev in /dev/video*; do
  [[ -e "$dev" ]] || continue
  DEVICE_ARGS+=(--device "$dev:$dev")
done
if ((${#DEVICE_ARGS[@]} == 0)); then
  warn "/dev/video* が見つかりません。USB カメラがマウントされません"
fi

if "${DOCKER[@]}" ps -a --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
  info "既存のコンテナ ${CONTAINER_NAME} を停止/削除"
  "${DOCKER[@]}" rm -f "$CONTAINER_NAME" >/dev/null
fi

info "DeepStream イメージ (${IMAGE_NAME}) を取得"
"${DOCKER[@]}" pull "$IMAGE_NAME"

RUN_SCRIPT=/workspace/edge-room-monitor/scripts/run_in_container.sh
if [ ! -x "$REPO_ROOT/scripts/run_in_container.sh" ]; then
  err "$RUN_SCRIPT が存在しないか実行不可です"
  exit 1
fi

SETUP_SCRIPT=/workspace/edge-room-monitor/scripts/run_in_container.sh

start_container() {
  info "コンテナ ${CONTAINER_NAME} を起動"
  local extra_args=()
  if [[ "$PRIVILEGED_MODE" == "true" ]]; then
    extra_args+=(--privileged)
  fi

  "${DOCKER[@]}" run -d \
    --name "$CONTAINER_NAME" \
    --runtime "$CONTAINER_RUNTIME" \
    --network "$DOCKER_NETWORK" \
    -e APP_HTTP_PORT="$APP_HTTP_PORT" \
    -v "$REPO_ROOT:/workspace/edge-room-monitor" \
    "${DEVICE_ARGS[@]}" \
    "${extra_args[@]}" \
    "$IMAGE_NAME" \
    bash -lc "$SETUP_SCRIPT"
}

stop_container() {
  if "${DOCKER[@]}" ps --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
    info "コンテナ ${CONTAINER_NAME} を停止"
    "${DOCKER[@]}" stop "$CONTAINER_NAME" >/dev/null || true
  fi
  if "${DOCKER[@]}" ps -a --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
    info "コンテナ ${CONTAINER_NAME} を削除"
    "${DOCKER[@]}" rm "$CONTAINER_NAME" >/dev/null || true
  fi
}

trap stop_container INT TERM
start_container
info "アプリログをフォローします (Ctrl+C で停止)"
"${DOCKER[@]}" logs -f "$CONTAINER_NAME"
