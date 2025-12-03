# モデル成果物

このディレクトリは `scripts/prepare_models.sh` によって自動的に更新されます。
スクリプトは YOLOv8n の ONNX ファイルを再利用（またはダウンロード）し、`trtexec`
で TensorRT エンジン（`yolov8n_fp16.engine`）を生成します。生成物は Git 管理外に
してあり、環境ごとに作り直せるようになっています。

実行後の想定レイアウトは次のとおりです。

```
models/
  yolov8n/
    yolov8n.onnx
    yolov8n_fp16.engine
```

### ONNX ファイルの用意方法

1. `models/yolov8n/yolov8n.onnx` が既に存在すれば、そのファイルをそのまま使用します。
2. 存在しない場合は、以下のいずれかに書かれたダウンロード URL を順に参照します。
   1. `models/yolov8n/yolov8n.onnx.url`（ローカル専用・Git 無視）
   2. `models/yolov8n.onnx.url`（共有設定・Git 管理）
3. どちらにも URL が無い場合は、`ONNX_URL=...` を環境変数で指定してください。

`.onnx.url` ファイルにはダイレクトにダウンロードできる URL を 1 行だけ記述します
（例: Hugging Face の `.../resolve/...` 形式）。ファイルが無く URL も指定されていない場合は
スクリプトがエラーで停止し、手動での配置を促します。

> 参考: 公式の `.pt` モデル  
> `https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt`

Jetson 内で PyTorch/Ultralytics を導入すると環境が複雑になるため、**PC など別環境で
上記 `.pt` を `ultralytics` CLI で ONNX に変換し、`models/yolov8n/` に配置


通常はスクリプトを手動実行する必要はありません。`start_app.sh` がコンテナ起動時に
`scripts/run_in_container.sh` を通じて自動実行します。生成物を削除したい場合は
`models/yolov8n` ディレクトリを消し、再度 `start_app.sh` を実行してください。
