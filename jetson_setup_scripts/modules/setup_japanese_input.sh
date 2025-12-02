#!/usr/bin/env bash
set -euo pipefail

sudo apt install -y ibus-mozc mozc-utils-gui
im-config -n ibus || true
gsettings set org.freedesktop.ibus.general preload-engines "['mozc-jp']" || true
gsettings set org.gnome.desktop.input-sources sources "[('ibus', 'mozc-jp')]" || true
ibus restart || true
