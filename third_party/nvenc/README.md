# NVENC API ヘッダ

- ファイル: `include/nvEncodeAPI.h`
- 取得元: https://github.com/FFmpeg/nv-codec-headers （タグ `n13.1.15.0`、NVIDIA Video Codec SDK 13.1.15 に相当）
- ライセンス: ヘッダの先頭に記載の MIT ライセンス（Copyright (c) 2010-2026 NVIDIA Corporation）
- 必要な NVIDIA ドライバ: Windows 610.0 以上
- 実行時は NVIDIA ドライバに含まれる `nvEncodeAPI64.dll` を `LoadLibrary` で読み込む（リンクするライブラリは無い）
