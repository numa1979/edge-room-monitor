# Edge Room Monitor - Person Detection & Tracking

DeepStream 6.0.1とYOLOv8nを使用したJetson Nano向けリアルタイム人物検出・追跡システム

## 現在の機能

- ✅ YOLOv8nによる人物検出（カスタムパーサー実装）
- ✅ nvtrackerによる人物追跡（各人物にIDを割り当て）
- ✅ MJPEGストリーミング（http://[jetson-ip]:8080）
- ✅ バウンディングボックスとラベルの表示

## システム要件

- Jetson Nano 4GB
- DeepStream 6.0.1
- CUDA 10.2
- TensorRT 8.0
- GStreamer 1.14.5

## ファイル構成

```
edge-room-monitor/
├── src/
│   ├── main.cpp              # メインアプリケーション
│   └── yolov8_parser.cpp     # カスタムYOLOv8パーサー
├── configs/
│   ├── camera_infer.pipeline      # 推論パイプライン（使用中）
│   ├── camera_preview.pipeline    # プレビュー専用パイプライン
│   ├── yolov8n_infer_config.txt  # YOLOv8推論設定
│   ├── nvtracker_config.yml      # トラッカー設定
│   └── coco_labels.txt           # COCOクラスラベル
├── models/
│   └── yolov8n/
│       ├── yolov8n.onnx          # YOLOv8n ONNXモデル
│       └── yolov8n_fp16.engine   # TensorRTエンジン（自動生成）
├── scripts/
│   ├── prepare_models.sh         # モデル準備スクリプト
│   └── run_in_container.sh       # コンテナ実行スクリプト
├── ui/
│   └── mjpeg_viewer.html         # Webビューア
├── CMakeLists.txt                # ビルド設定
└── start_app.sh                  # アプリ起動スクリプト
```

## 使用方法

### 1. アプリケーションの起動

```bash
cd /home/numa/edge-room-monitor/edge-room-monitor
sudo ./start_app.sh
```

### 2. プレビューの確認

ブラウザで以下にアクセス：
```
http://192.168.0.212:8080
```

### 3. 停止

```bash
Ctrl+C
```

## 設定

### 推論間隔の調整

`configs/yolov8n_infer_config.txt`:
```
interval=2  # 2フレームに1回推論（0=全フレーム）
```

### 検出閾値の調整

`src/yolov8_parser.cpp`:
```cpp
const float confThreshold = 0.5f;  // 0.0-1.0
```

### トラッカー設定

`configs/nvtracker_config.yml`:
```yaml
tracking-min-conf: 0.3  # 追跡開始の最小信頼度
tracking-min-bbox-area: 100  # 追跡する最小バウンディングボックス面積
```

## パフォーマンス

- 解像度: 640x640
- FPS: 約15fps（interval=2）
- 推論時間: 約60-70ms/フレーム
- メモリ使用量: 約2.5GB

## トラブルシューティング

### カメラが起動しない
```bash
# カメラデバイスを確認
ls -l /dev/video*
```

### フレームドロップが多い
- `interval`を増やす（例：interval=3）
- 解像度を下げる（pipeline内のwidth/heightを変更）

### セグメンテーションフォルト
- 現在の設定で安定動作を確認済み
- パーサーやパイプラインを変更する場合は慎重に

## 次の開発予定

1. タップ・ツー・トラック機能
   - Webインターフェースで人物をクリック
   - 選択された人物を特別に追跡

2. 再識別（Re-ID）
   - 特徴抽出（nvsecondarygie）
   - フレームアウト後の再認識

3. イベント記録
   - 人物の入退室記録
   - タイムスタンプ付きログ

## 技術的な詳細

### カスタムYOLOv8パーサー

- YOLOv8の出力形式: `[1, 84, 8400]`
  - 84 = 4 bbox座標 + 80クラススコア
  - 8400 = アンカーポイント数
- 座標は0-1の正規化値（640倍してピクセル座標に変換）
- 人物クラス（class 0）のみをフィルタリング
- カスタムNMS実装（IOU閾値: 0.45）

### パイプライン構成

```
v4l2src → jpegdec → videoconvert → nvvideoconvert → 
nvstreammux → nvinfer → nvtracker → nvvideoconvert → 
nvdsosd → nvvideoconvert → videoconvert → jpegenc → appsink
```

## ライセンス

このプロジェクトは開発中です。

## 作成日

2025年12月4日
