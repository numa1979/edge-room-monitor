#!/usr/bin/env bash
set -euo pipefail

sudo apt update
sudo apt install -y \
  build-essential dkms git curl wget \
  ca-certificates libnss3 libnss3-tools \
  python3 python3-venv python3-pip \
  libgl1 \
  chromium-codecs-ffmpeg-extra \
  avahi-daemon

# mDNS で <hostname>.local へ名前解決できるようにする
sudo systemctl enable --now avahi-daemon

# ホスト名を固定（デフォルト jetson）。環境変数 HOSTNAME_OVERRIDE で変更可
HOSTNAME_OVERRIDE=${HOSTNAME_OVERRIDE:-jetson}
CURRENT_HOSTNAME=$(hostname)
if [ "$CURRENT_HOSTNAME" != "$HOSTNAME_OVERRIDE" ]; then
  sudo hostnamectl set-hostname "$HOSTNAME_OVERRIDE"
  # /etc/hosts にも反映
  sudo sed -i "s/127\\.0\\.1\\.1\\s\\+.*/127.0.1.1\t${HOSTNAME_OVERRIDE}/" /etc/hosts || true
  echo "127.0.1.1	${HOSTNAME_OVERRIDE}" | sudo tee -a /etc/hosts >/dev/null
  sudo systemctl restart avahi-daemon
fi
