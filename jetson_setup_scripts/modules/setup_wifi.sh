#!/usr/bin/env bash
# Install rtl88x2bu driver if needed and connect to Wi-Fi.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
VENDOR_DRIVER_DIR=${VENDOR_DRIVER_DIR:-$REPO_ROOT/vendor/rtl88x2bu}
VENDOR_ARCHIVE=${VENDOR_ARCHIVE:-$REPO_ROOT/vendor/rtl88x2bu.tar.gz}
VENDOR_COMMIT_FILE=${VENDOR_COMMIT_FILE:-$REPO_ROOT/vendor/rtl88x2bu.COMMIT}
DRIVER_REPO=${DRIVER_REPO:-https://github.com/cilynx/rtl88x2bu}
DRIVER_COMMIT=${DRIVER_COMMIT:-$(cat "$VENDOR_COMMIT_FILE" 2>/dev/null || echo 42ec4de8d36c9eac0ac26ae714837efbf1a09c1d)}
DRIVER_DIR=${DRIVER_DIR:-$HOME/rtl88x2bu}
IFACE=${IFACE:-wlan0}
SSID=${SSID:-}
PASS=${PASS:-}
CONFIG_FILE=${WIFI_CONFIG:-$REPO_ROOT/wifi_config}

info() { printf '[wifi] %s\n' "$*"; }
warn() { printf '[wifi warn] %s\n' "$*" >&2; }

need_driver_install() {
  if modinfo 88x2bu >/dev/null 2>&1; then
    return 1
  fi
  return 0
}

prepare_driver_sources() {
  mkdir -p "$HOME" "$(dirname "$DRIVER_DIR")"
  if [ -d "$DRIVER_DIR" ]; then
    info "既存のドライバソースを利用します: ${DRIVER_DIR}"
    return
  fi

  if [ -d "$VENDOR_DRIVER_DIR" ]; then
    info "リポジトリ同梱の rtl88x2bu をコピーします"
    cp -R "$VENDOR_DRIVER_DIR" "$DRIVER_DIR"
    return
  fi

  if [ -f "$VENDOR_ARCHIVE" ]; then
    info "リポジトリ同梱の rtl88x2bu を展開します (${VENDOR_ARCHIVE})"
    mkdir -p "$DRIVER_DIR"
    tar -xzf "$VENDOR_ARCHIVE" -C "$DRIVER_DIR" --strip-components=1
    return
  fi

  info "同梱版が見つからないため ${DRIVER_REPO}@${DRIVER_COMMIT} を取得します"
  git clone --depth 1 "$DRIVER_REPO" "$DRIVER_DIR"
  if [ -n "$DRIVER_COMMIT" ]; then
    (cd "$DRIVER_DIR" && git checkout "$DRIVER_COMMIT" >/dev/null 2>&1) || warn "指定のコミット (${DRIVER_COMMIT}) への切替に失敗しました"
  fi
}

install_driver() {
  info "rtl88x2bu ドライバを準備します"
  prepare_driver_sources
  cd "$DRIVER_DIR"
  make clean || true
  make ARCH=arm64
  sudo make install
  sudo modprobe 88x2bu
}

if need_driver_install; then
  install_driver
else
  info "既に rtl88x2bu ドライバが導入済みのためスキップ"
  sudo modprobe 88x2bu || true
fi

load_config() {
  if [ ! -f "$CONFIG_FILE" ]; then
    return
  fi
  info "Wi-Fi 設定を ${CONFIG_FILE} から読み込みます"
  while IFS='=' read -r key value; do
    case "$key" in
      ''|\#*) continue ;;
      SSID) : "${SSID:=$value}" ;;
      PASS) : "${PASS:=$value}" ;;
      IFACE) : "${IFACE:=$value}" ;;
    esac
  done < "$CONFIG_FILE"
}

load_config

if [ -z "$SSID" ]; then
  read -rp "SSID: " SSID
fi
if [ -z "$PASS" ]; then
  read -rsp "Password: " PASS
  echo
fi

echo "[wifi] Wi-Fi SSID='${SSID}' へ接続します"
if ! sudo nmcli device wifi connect "$SSID" password "$PASS" ifname "$IFACE"; then
  warn "Wi-Fi への接続に失敗しましたが処理を継続します。SSID/PASS/IFACE を確認してください"
fi
