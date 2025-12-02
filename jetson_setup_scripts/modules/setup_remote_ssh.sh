#!/usr/bin/env bash
# Enable SSH access into the Ubuntu 22.04 container for VS Code Remote-SSH.
set -euo pipefail

CONTAINER_NAME=${CONTAINER_NAME:-jetson-watchdog-ubuntu2204}
SSH_PORT=${SSH_PORT:-2222}
HOST_USER=${HOST_USER:-${SUDO_USER:-$USER}}
CONTAINER_USER=${CONTAINER_USER:-$HOST_USER}
CONTAINER_PASS=${CONTAINER_PASS:-}
HOST_UID=$(id -u "$HOST_USER")
HOST_HOME=$(eval echo "~${HOST_USER}")

info() { printf '\033[1;34m[ssh]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[ssh ERROR]\033[0m %s\n' "$*" >&2; }

command -v docker >/dev/null 2>&1 || { err "docker コマンドがありません"; exit 1; }
DOCKER=(docker)
if ! "${DOCKER[@]}" ps >/dev/null 2>&1; then
  if sudo docker ps >/dev/null 2>&1; then
    info "docker へのアクセスに sudo を使用します"
    DOCKER=(sudo docker)
  else
    err "docker デーモンにアクセスできません。先に docker グループや sudo 権限を確認してください"
    exit 1
  fi
fi

if ! "${DOCKER[@]}" ps -a --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
  err "コンテナ ${CONTAINER_NAME} が存在しません。先に Wi-Fi/Docker セットアップを実行してください。"
  exit 1
fi

if ! "${DOCKER[@]}" ps --format '{{.Names}}' | grep -Fxq "$CONTAINER_NAME"; then
  info "コンテナ ${CONTAINER_NAME} を起動"
  "${DOCKER[@]}" start "$CONTAINER_NAME" >/dev/null
fi

info "openssh-server と sudo を導入"
"${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server sudo locales"

"${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "mkdir -p /var/run/sshd && sed -i 's/^#\\?PermitRootLogin.*/PermitRootLogin no/' /etc/ssh/sshd_config"
"${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "sed -i 's/^#\\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config"

info "ユーザ ${CONTAINER_USER}(uid=${HOST_UID}) を準備"
if ! "${DOCKER[@]}" exec "$CONTAINER_NAME" id -u "$CONTAINER_USER" >/dev/null 2>&1; then
  # UID が既に他ユーザに使われている場合はそのユーザを流用する
  if EXISTING_USER=$("${DOCKER[@]}" exec "$CONTAINER_NAME" sh -c "getent passwd ${HOST_UID} | cut -d: -f1"); then
    if [ -n "$EXISTING_USER" ]; then
      if [ "$EXISTING_USER" != "$CONTAINER_USER" ]; then
        info "uid=${HOST_UID} は既存ユーザ ${EXISTING_USER} が使用中のため ${CONTAINER_USER} にリネームします"
        "${DOCKER[@]}" exec "$CONTAINER_NAME" usermod -l "$CONTAINER_USER" "$EXISTING_USER"
        "${DOCKER[@]}" exec "$CONTAINER_NAME" usermod -d "/home/${CONTAINER_USER}" -m "$CONTAINER_USER"
      else
        info "uid=${HOST_UID} は既存ユーザ ${EXISTING_USER} が使用中のため流用します"
      fi
    fi
  fi
fi

if ! "${DOCKER[@]}" exec "$CONTAINER_NAME" id -u "$CONTAINER_USER" >/dev/null 2>&1; then
  "${DOCKER[@]}" exec "$CONTAINER_NAME" useradd -m -u "$HOST_UID" -s /bin/bash -G sudo "$CONTAINER_USER"
fi

if [ -z "$CONTAINER_PASS" ]; then
  read -rsp "${CONTAINER_USER} のパスワード: " CONTAINER_PASS
  echo
fi
echo "${CONTAINER_USER}:${CONTAINER_PASS}" | "${DOCKER[@]}" exec -i "$CONTAINER_NAME" chpasswd

SSH_DIR="/home/${CONTAINER_USER}/.ssh"
"${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "mkdir -p ${SSH_DIR} && chmod 700 ${SSH_DIR} && chown -R ${CONTAINER_USER}:${CONTAINER_USER} ${SSH_DIR}"
if [ -f "${HOST_HOME}/.ssh/id_ed25519.pub" ]; then
  info "ホスト鍵を authorized_keys に追加"
  "${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "touch ${SSH_DIR}/authorized_keys && chmod 600 ${SSH_DIR}/authorized_keys"
  "${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "cat '${HOST_HOME}/.ssh/id_ed25519.pub' >> ${SSH_DIR}/authorized_keys"
  "${DOCKER[@]}" exec "$CONTAINER_NAME" bash -lc "chown ${CONTAINER_USER}:${CONTAINER_USER} ${SSH_DIR}/authorized_keys"
else
  info "${HOST_HOME}/.ssh/id_ed25519.pub が無いため鍵コピーをスキップ"
fi

"${DOCKER[@]}" exec "$CONTAINER_NAME" service ssh restart >/dev/null
info "完了: VS Code から ssh ${CONTAINER_USER}@<Jetson IP> -p ${SSH_PORT} で接続可能"
