#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
APP_START_SCRIPT="$REPO_ROOT/edge-room-monitor/start_app.sh"

info() { printf '\033[1;34m[app-service]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[app-service ERROR]\033[0m %s\n' "$*" >&2; }

if [ ! -f "$APP_START_SCRIPT" ]; then
  err "アプリ起動スクリプトが見つかりません: $APP_START_SCRIPT"
  exit 1
fi

SERVICE_NAME="edge-room-monitor"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

info "systemd サービスを作成: ${SERVICE_NAME}"
sudo tee "$SERVICE_FILE" >/dev/null <<EOF
[Unit]
Description=Edge Room Monitor Application
After=network-online.target docker.service
Wants=network-online.target
Requires=docker.service

[Service]
Type=simple
User=$USER
WorkingDirectory=$REPO_ROOT/edge-room-monitor
ExecStart=$APP_START_SCRIPT
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

info "systemd サービスを有効化"
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"

info "完了: Jetson 起動時に ${SERVICE_NAME} が自動起動します"
info "手動起動: sudo systemctl start ${SERVICE_NAME}"
info "ログ確認: sudo journalctl -u ${SERVICE_NAME} -f"
