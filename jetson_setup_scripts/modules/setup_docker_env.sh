#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

CONTAINER_NAME=${CONTAINER_NAME:-jetson-watchdog-ubuntu2204}
IMAGE_NAME=${IMAGE_NAME:-ubuntu:22.04}
HOST_PORT=${HOST_PORT:-8080}
CONTAINER_PORT=${CONTAINER_PORT:-8080}
SSH_PORT=${SSH_PORT:-2222}
VENV_PATH=${VENV_PATH:-/opt/jetson_watchdog_venv}
SKIP_APT_INSTALL=${SKIP_APT_INSTALL:-0}
PIP_WHEEL_DIR=${PIP_WHEEL_DIR:-}
OFFLINE_INSTALL=${OFFLINE_INSTALL:-0}
DEFAULT_WHEEL_DIR=${DEFAULT_WHEEL_DIR:-$REPO_ROOT/vendor/wheels}

if [[ "$OFFLINE_INSTALL" == "1" ]]; then
  info "OFFLINE_INSTALL=1: apt install をスキップし、ローカルホイールを優先"
  SKIP_APT_INSTALL=1
  if [[ -z "$PIP_WHEEL_DIR" ]]; then
    PIP_WHEEL_DIR="$DEFAULT_WHEEL_DIR"
  fi
fi
APP_ENTRY="TORCH_LOAD_WEIGHTS_ONLY=0 uvicorn app.main:app --host 0.0.0.0 --port ${CONTAINER_PORT} > /workspace/uvicorn.log 2>&1"

info() { printf '\033[1;34m[docker]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[docker ERROR]\033[0m %s\n' "$*" >&2; }

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
      -p "${HOST_PORT}:${CONTAINER_PORT}" \
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

exec_root() {
  "${DOCKER[@]}" exec -u root "$CONTAINER_NAME" bash -lc "$1"
}

prepare_runtime() {
  info "Python 実行環境を準備"
  if [[ "$SKIP_APT_INSTALL" == "1" ]]; then
    info "SKIP_APT_INSTALL=1 のため apt-get update/install をスキップ"
  else
    exec_root "apt-get update"
    exec_root "DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-venv python3-pip git curl libgl1"
  fi

  exec_root "mkdir -p /workspace && test -d ${VENV_PATH} || python3 -m venv ${VENV_PATH}"
  local use_local_wheels=0
  if [[ -n "$PIP_WHEEL_DIR" ]]; then
    if [[ -d "$PIP_WHEEL_DIR" ]]; then
      use_local_wheels=1
    else
      err "指定された PIP_WHEEL_DIR=${PIP_WHEEL_DIR} が存在しません"
      exit 1
    fi
  fi

  if [[ "$use_local_wheels" == "1" ]]; then
    info "ローカルホイール ${PIP_WHEEL_DIR} から pip install (--no-index)"
  else
    if ! exec_root "source ${VENV_PATH}/bin/activate && pip install --upgrade pip"; then
      info "pip install --upgrade pip に失敗しました。ネットワークまたはAPTキャッシュを確認してください。"
    fi
  fi

  if [[ "$use_local_wheels" == "1" ]]; then
    exec_root "cd /workspace && source ${VENV_PATH}/bin/activate && pip install --no-index --find-links ${PIP_WHEEL_DIR} -r requirements.txt"
  else
    if ! exec_root "cd /workspace && source ${VENV_PATH}/bin/activate && pip install -r requirements.txt"; then
      if [[ -d "$DEFAULT_WHEEL_DIR" ]]; then
        info "オンライン pip install に失敗。${DEFAULT_WHEEL_DIR} から再試行します"
        exec_root "cd /workspace && source ${VENV_PATH}/bin/activate && pip install --no-index --find-links ${DEFAULT_WHEEL_DIR} -r requirements.txt"
      else
        err "pip install -r requirements.txt に失敗しました。PIP_WHEEL_DIR か DEFAULT_WHEEL_DIR を準備してください。"
        exit 1
      fi
    fi
  fi
}

start_app() {
  info "既存アプリを停止"
  "${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "pkill -f 'uvicorn app.main:app'" >/dev/null 2>&1 || true
  info "FastAPI を起動"
  info "YOLOv8 モデルを確認/ダウンロード"
  if ! "${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "cd /workspace && source ${VENV_PATH}/bin/activate && python scripts/download_yolo_weights.py"; then
    info "YOLO ダウンロードに失敗しました。あとで手動実行してください"
  fi
  "${DOCKER[@]}" exec -d "$CONTAINER_NAME" bash -lc "cd /workspace && source ${VENV_PATH}/bin/activate && ${APP_ENTRY}"
}

start_container
prepare_runtime
start_app

info "完了: http://<Jetson IP>:${HOST_PORT} でアクセス可 (SSH:${SSH_PORT})"
