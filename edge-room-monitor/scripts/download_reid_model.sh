#!/bin/bash
set -e

MODELS_DIR="/workspace/edge-room-monitor/models"
REID_DIR="${MODELS_DIR}/resnet50_reid"

echo "Re-ID モデルのダウンロード準備..."

# Re-IDモデル用ディレクトリ作成
mkdir -p "${REID_DIR}"

# NVIDIAのRe-IDモデル（ResNet50ベース）をダウンロード
# 注: 実際のモデルURLは要確認
echo "Re-IDモデルをダウンロード中..."

# Option 1: torchreid からエクスポート
# Option 2: NVIDIA TAO Toolkit のモデル
# Option 3: 自前でONNXモデルを用意

# ここでは、簡易的にResNet50の特徴抽出部分を使用
# 実際には、Person Re-ID用に学習されたモデルが必要

cat > "${REID_DIR}/README.md" << 'EOF'
# Re-ID Model Setup

## Option 1: Use Pre-trained ResNet50 for Feature Extraction

DeepStreamでは、分類モデルの最終層前の特徴ベクトルを取得できます。

## Option 2: Download Person Re-ID Model

Person Re-ID用に学習されたモデルを使用する場合：

1. NVIDIA TAO Toolkit からダウンロード
2. torchreid からエクスポート
3. 自前で学習したモデルを使用

## 必要なファイル

- resnet50_reid.onnx (または .etlt)
- resnet50_reid_config.txt (nvinfer設定)

## 特徴ベクトル

- 出力: 2048次元または512次元の特徴ベクトル
- 正規化: L2ノルムで正規化
- 比較: コサイン類似度またはユークリッド距離
EOF

echo "Re-IDモデルの準備が必要です。"
echo "詳細は ${REID_DIR}/README.md を参照してください。"
