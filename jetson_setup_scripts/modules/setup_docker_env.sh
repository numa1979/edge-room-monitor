#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

CONTAINER_NAME=${CONTAINER_NAME:-jetson-watchdog-ubuntu2204}
IMAGE_NAME=${IMAGE_NAME:-ubuntu:22.04}
SSH_PORT=${SSH_PORT:-2222}

info() { printf '\033[1;34m[docker]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[docker ERROR]\033[0m %s\n' "$*" >&2; }

command -v docker >/dev/null 2>&1 || { err "docker コマンドがありません。"; exit 1; }
DOCKER=(docker)
if ! "${DOCKER[@]}" ps >/dev/null 2>&1; then
  if sudo docker ps >/dev/null 2>&1; then
    info "docker へのアクセスに sudo を使用します"
    DOCKER=(sudo docker)
  else
    err "docker デーモンにアクセスできません。ユーザーを docker グループに追加するか sudo 権限を確認してください"
    exit 1
  fi
fi

container_exists() {
  "${DOCKER[@]}" ps -a --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"
}

start_container() {
  local device_args=()
  for dev in /dev/video*; do
    [[ -e "$dev" ]] || continue
    device_args+=("--device" "${dev}:${dev}")
  done

  if ((${#device_args[@]} == 0)); then
    info "/dev/video* が見つかりません。カメラをコンテナへ渡せません。"
  fi

  if ! container_exists; then
    info "Ubuntu 22.04 コンテナ ${CONTAINER_NAME} を生成"
    "${DOCKER[@]}" pull "$IMAGE_NAME"
    "${DOCKER[@]}" create \
      --name "$CONTAINER_NAME" \
      --hostname "$CONTAINER_NAME" \
      --restart unless-stopped \
      -p "${SSH_PORT}:22" \
      -v "$REPO_ROOT:/workspace" \
      "${device_args[@]}" \
      -w /workspace \
      "$IMAGE_NAME" \
      bash -lc "service ssh start >/dev/null 2>&1 || true; tail -f /dev/null" >/dev/null
  fi

  if ! "${DOCKER[@]}" ps --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
    info "コンテナ ${CONTAINER_NAME} を起動"
    "${DOCKER[@]}" start "$CONTAINER_NAME" >/dev/null
  fi
}

start_container
info "完了: SSH:${SSH_PORT} で 22.04 コンテナに接続可能（/workspace を開く）"
