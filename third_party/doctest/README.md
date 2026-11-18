# doctest

- ファイル: `doctest/doctest.h`（単一ヘッダ）、`LICENSE.txt`
- 取得元: https://github.com/doctest/doctest （タグ `v2.5.3`）
- ライセンス: MIT（`LICENSE.txt`）
- 用途: 単体テスト（`shared/*/tests`、`quest/tests`）。製品のバイナリには入らない
- テストの `main` は `shared/testing/test_main.cpp` にある（コンソールの文字コードを UTF-8 にしてから doctest を呼ぶ）
