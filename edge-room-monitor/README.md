# 患者見守りシステム - Edge Room Monitor

DeepStream 6.0.1 + YOLOv8nを使用したJetson Nano向けリアルタイム人物追跡・異常検知システム

## 概要

病院・介護施設向けの自動見守りシステム。最大4人まで自動追跡し、横たわり継続などの異常行動を検知してアラートを発報。

## 主な機能

### 自動追跡（最大4人）
- 人を検出したら自動で追跡開始
- 手動モードに切り替えてタップで登録/解除も可能
- 60秒間は見失っても追跡維持

### 異常検知
- **横たわり継続**: 20秒以上横たわった状態が継続

### 追跡モード
- **自動モード**: 最初の4人を自動追跡、タップで解除
- **手動モード**: タップで登録/解除を手動選択（8人いて4人だけ追跡したい場合）

## システム要件

- Jetson Nano 4GB Development Kit
- JetPack 4.6.1
- DeepStream 6.0.1
- CUDA 10.2
- TensorRT 8.0
- GStreamer 1.14.5
- USBカメラ（640x480, 30fps推奨）

## ファイル構成

```
edge-room-monitor/
├── src/
│   ├── main.cpp              # メインアプリケーション
│   └── yolov8_parser.cpp     # カスタムYOLOv8パーサー
├── configs/
│   ├── camera_infer.pipeline      # 推論パイプライン
│   ├── yolov8n_infer_config.txt  # YOLOv8推論設定
│   ├── nvtracker_config.yml      # トラッカー設定
│   └── coco_labels.txt           # COCOクラスラベル
├── models/
│   └── yolov8n/
│       ├── yolov8n.onnx          # YOLOv8n ONNXモデル
│       └── yolov8n_fp16.engine   # TensorRTエンジン（自動生成）
├── ui/
│   └── monitor.html              # 見守りUI
├── CMakeLists.txt                # ビルド設定
└── start_app.sh                  # アプリ起動スクリプト
```

## 使用方法

### 起動

```bash
cd ~/edge-room-monitor/edge-room-monitor
sudo ./start_app.sh
```

### アクセス

ブラウザで以下にアクセス：
```
http://192.168.0.212:8080
```

### 操作

1. **モード切替**: 画面上部の緑/オレンジバーをクリック
2. **自動モード**: 自動で追跡開始、タップで解除
3. **手動モード**: タップで登録/解除

### 停止

```bash
Ctrl+C
```

または

```bash
sudo docker stop edge-room-monitor-app
```

## API

### GET /api/detections
検出情報を取得

```json
{
  "detections": [
    {
      "nvtracker_id": 5,
      "fixed_id": 0,
      "registered": true,
      "class_id": 0,
      "confidence": 0.85,
      "bbox": {"left": 100, "top": 50, "width": 200, "height": 400}
    }
  ]
}
```

### GET /api/alerts
アラート一覧を取得

```json
{
  "alerts": [
    {
      "index": 0,
      "fixed_id": 0,
      "type": 4,
      "message": "Lying for 20+ seconds",
      "timestamp": 1701234567890,
      "acknowledged": false
    }
  ]
}
```

### GET /api/config
現在の設定を取得

```json
{
  "auto_register": true
}
```

### POST /api/register
手動登録（手動モード時）

```json
{"nvtracker_id": 5}
```

### POST /api/unregister
追跡解除

```json
{"nvtracker_id": 5}
```

### POST /api/toggle_auto_register
自動/手動モード切替

### POST /api/clear
全員の追跡を解除

### POST /api/clear_alerts
アラートをクリア

## 設定調整

### YOLOv8信頼度閾値

`src/yolov8_parser.cpp`:
```cpp
const float confThreshold = 0.35f;  // 0.35 = 35%
```

### 推論間隔

`configs/yolov8n_infer_config.txt`:
```
interval=2  # 2フレームに1回推論（0=全フレーム）
```

### 横たわり検知時間

`src/main.cpp`:
```cpp
if (lying_sec >= 20 && lying_sec < 21) {  // 20秒
```

### nvtracker追跡維持時間

`configs/nvtracker_config.yml`:
```yaml
maxShadowTrackingAge: 900  # 900フレーム = 約60秒
```

## パフォーマンス

- 解像度: 640x640
- FPS: 約15fps（interval=2）
- 推論時間: 約60-70ms/フレーム
- メモリ使用量: 約2.5GB
- 最大追跡人数: 4人（Jetson Nano性能考慮）

## カラーコード

- **緑**: 患者0
- **シアン**: 患者1
- **黄**: 患者2
- **マゼンタ**: 患者3
- **赤**: 未登録

## 推奨カメラ配置

- **俯瞰視点**: 斜め上から見下ろす（45度程度）
- **高さ**: 2.5〜3m
- **照明**: 明るい環境
- **視野**: 監視対象全体を含む

## トラブルシューティング

### カメラが起動しない
```bash
ls -l /dev/video*
```

### 処理が重い
- `interval`を増やす（2 → 3）
- YOLOv8閾値を上げる（0.35 → 0.45）

### 誤検出が多い
- YOLOv8閾値を上げる（0.35 → 0.45）
- 照明を改善

### 横になると見失う
- YOLOv8閾値を下げる（0.35 → 0.30）
- 照明を明るくする

## 技術的な詳細

### カスタムYOLOv8パーサー

- YOLOv8の出力形式: `[1, 84, 8400]`
  - 84 = 4 bbox座標 + 80クラススコア
  - 8400 = アンカーポイント数
- 人物クラス（class 0）のみをフィルタリング
- カスタムNMS実装（IOU閾値: 0.45）

### パイプライン構成

```
v4l2src → jpegdec → videoconvert → nvvideoconvert → 
nvstreammux → nvinfer(YOLOv8) → nvtracker → 
nvvideoconvert → nvdsosd → nvvideoconvert → 
videoconvert → jpegenc → appsink
```

### 異常検知ロジック

- 横たわり判定: `width > height * 1.8`
- 追跡維持: 60秒間見失っても継続
- アラート: 横たわり20秒継続で発報

## 制限事項

- 最大4人まで同時追跡（Jetson Nano性能制約）
- Re-ID機能なし（フレームアウト後の自動再識別不可）
- 単一カメラのみ
- 転倒検知は誤検知が多いため無効化

## 今後の拡張（ハードウェアアップグレード時）

Jetson Orin Nanoにアップグレードすれば：
- DeepStream 6.1以降が使える
- 本格的なRe-ID特徴抽出が可能
- フレームアウト後も自動再識別
- 追跡人数を増やせる（8人以上）

## ログ確認

```bash
# 全ログ
sudo docker logs -f edge-room-monitor-app

# アラートのみ
sudo docker logs -f edge-room-monitor-app 2>&1 | grep "\[Alert\]"

# 自動登録のみ
sudo docker logs -f edge-room-monitor-app 2>&1 | grep "\[Auto\]"
```

## クリーンビルド

```bash
cd ~/edge-room-monitor/edge-room-monitor
sudo docker stop edge-room-monitor-app
sudo docker rm edge-room-monitor-app
sudo rm -rf build
sudo ./start_app.sh
```

## ライセンス

このプロジェクトは開発中です。

## 作成日

2025年12月5日
