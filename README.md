# WN-VRBridge

SteamVR の映像と音声を Wi-Fi で Meta Quest 3 に送り、Quest 3 の頭とコントローラの動きを SteamVR に返すアプリです。PC と Quest を同じネットワークにつなぎ、Quest のアプリを起動するだけで SteamVR が起動して使えます。

## 動作環境
| 項目 | 内容 |
|---|---|
| OS | Windows 11（64 ビット） |
| GPU | AV1 のハードウェアエンコードに対応した NVIDIA の GPU（GeForce RTX 40 シリーズ以降） |
| SteamVR | Steam から入れた SteamVR（最新の安定版） |
| ヘッドセット | Meta Quest 3（開発者モードを有効にしたもの） |
| ネットワーク | Wi-Fi 6 / 6E のアクセスポイントと PC を有線でつなぐ。Quest はそのアクセスポイントに Wi-Fi でつなぐ |

## 配布物の中身
```
WN-VRBridge-<版>/
  wnvrbridge_host.exe          ホスト（PC で常駐するアプリ）
  driver/wnvrbridge/            SteamVR のドライバ
  quest/WN-VRBridge-<版>.apk    Quest のアプリ
  README.md                     この説明
```
フォルダは好きな場所に置いてかまいません。ただし、ドライバを登録したあとにフォルダを動かすと SteamVR がドライバを見つけられなくなるので、動かしたら登録し直してください。

## 使い始めるまで

### 1. Quest にアプリを入れる
1. Quest 3 の開発者モードを有効にします（スマートフォンの Meta Horizon アプリから）。
2. Quest を USB で PC につなぎ、Quest に出る「USB デバッグを許可しますか？」を許可します。
3. PC で `adb` を使って APK を入れます。`adb` は Google の「Android SDK Platform-Tools」に入っています（SideQuest などのツールで入れてもかまいません）。
   - Android Studio を入れている場合は `%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe` にあります。
   - 入れていない場合は、Platform-Tools の zip を Google の Android Developers のサイトからダウンロードして展開します。
   - `adb` が PATH に無いときは、`adb.exe` の場所を含めて実行します。配布フォルダでコマンド プロンプトを開き、次のように実行します（Android Studio の場合の例）:
   ```
   "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" install -r quest\WN-VRBridge-<版>.apk
   ```
   PowerShell では次のようにします:
   ```
   & "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r quest\WN-VRBridge-<版>.apk
   ```
   「Success」と出れば入っています。
4. Quest のアプリ一覧の「提供元不明」に **WN-VRBridge** が出ます。

### 2. PC でホストを起動して、ドライバとファイアウォールを設定する
1. `wnvrbridge_host.exe` を起動します。WN-VRBridge の画面が開き、タスクトレイに六角形にゴーグルのアイコンが出ます。
   - 画面のいちばん上に、今の状態と次にすることが出ます。初めて起動したときは「セットアップが終わっていません」と出ます。
   - 画面を閉じても、ホストはタスクトレイで動き続けます。もう一度開くには、アイコンをダブルクリックします。
2. 「セットアップ」のタブで、次の 2 つを行います。
   - **1. SteamVR にドライバを登録する**: 「ドライバを登録する」を押します。SteamVR が動いているときは、起動し直すと反映されます。
   - **2. Windows ファイアウォールで通信を許可する**: 「許可ルールを登録する」を押します。Quest からの通信（UDP 9943・9944、プライベートとドメインのネットワーク）を許可するルールを加えます。管理者の権限が要るので、Windows の確認（UAC）が出ます。

   2 つとも済むと、タブの名前が「セットアップ（未完了）」から「セットアップ」に変わり、画面の上が「Quest の接続を待っています」になります。
3. 必要なら、「設定」のタブの「Windows の起動時に、このアプリを自動で起動する」にチェックを入れます（自動で起動したときは、画面を出さずにタスクトレイに入ります）。

### 3. Quest からつなぐ
1. Quest で WN-VRBridge を起動します。Quest の前に「PC を探しています」と出ます。
2. 初めてつなぐ Quest のときは、PC に確認のダイアログが出ます。「はい」を選ぶと、次からは確認なしでつながります（「設定」のタブの「接続を許可した Quest」で取り消せます）。
3. ホストが SteamVR を起動し、しばらくすると SteamVR の映像が出ます。ホストの画面の上は「Quest とつながっています」になり、「状態」のタブに遅延や通信の状況が出ます。
   - 「設定」のタブで「SteamVR を自動で起動する」をオフにしているときは、Quest に「SteamVR が起動していません」と出ます。トリガーを引くと起動します。

### 4. マイクを使う（使うときだけ）
Quest のマイクの音を、PC のマイクとして使えます。仮想オーディオケーブルの **VB-CABLE** を使います。
1. VB-Audio のサイト（https://vb-audio.com/Cable/ ）から VB-CABLE をダウンロードし、`VBCABLE_Setup_x64.exe` を管理者として実行して「Install Driver」を押します。終わったら PC を再起動します。
2. Quest からつなぐと、初めての 1 回だけ、Quest にマイクの使用の許可を求める画面が出ます。「許可」を選びます。
3. VRChat などのアプリで、マイクに「CABLE Output (VB-Audio Virtual Cable)」を選びます。

ホストの「状態」のタブの「マイク」が「Quest のマイクの音を CABLE Output に送っています」になっていれば使えています。

## 使い方
- **終わるとき**: SteamVR を終了するか、Quest のアプリを終了します。SteamVR を終了すると、Quest に「SteamVR が起動していません」と出ます。もう一度使うときは、Quest でトリガーを引きます。
  - 「設定」のタブで「SteamVR を自動で終了する」をオンにすると、Quest のアプリを終了してから 15 秒たってもつながらないときに、SteamVR も終了します（ヘッドセットを外しただけでは終了しません）。
- **音**: PC の音は Quest で鳴ります。つないでいるあいだは PC のスピーカーを消音にし、切断すると元に戻します（「設定」のタブで変えられます）。
- **ホストの画面**: タスクトレイのアイコンの右下に、状態の色の印が出ます（緑: Quest とつながっている、青: 準備中、灰: 待機中、黄: 操作が必要、赤: 失敗）。画面は 4 つのタブに分かれています。

  | タブ | できること |
  |---|---|
  | 状態 | PC の待ち受け・SteamVR・Quest・マイクの状態と、接続中の統計（遅延、コマ数、通信、音声） |
  | 設定 | 映像（ビットレート、Quest の描画の解像度、foveation）、SteamVR の自動起動・自動終了、音声（マイク、PC の音を Quest で鳴らすか、接続中の PC のスピーカーの消音）、待ち受けるネットワーク、Windows の起動時の自動起動、接続を許可した Quest の取り消し |
  | セットアップ | SteamVR へのドライバの登録と、ファイアウォールの許可ルール |
  | 情報 | ログと設定ファイルの場所、うまくいかないときの確認、ホストの終了 |

  設定は、変えるとすぐに保存されます（「保存」ボタンはありません）。ほとんどはその場で反映されますが、次のものは反映されるまでに手順が要ります。
  - **Quest の描画の解像度**: Quest のアプリを次に起動したときから使われます。
  - **foveation**（中心を細かく、周りを粗く送る）: 次に Quest がつながったときから使われます。
  - **待ち受けるネットワーク**: Quest とつながっていないときに切り替わります。

  描画の解像度か foveation を接続中に変えると、Quest に「設定が変わりました」と 5 秒ほど出ます。

- **画質**: 文字がつぶれる、全体がぼやけるときは、「設定」のタブで「Quest の描画の解像度」を上げるか、「周りを縮める比率」を小さく（1/2 など）してください。ビットレートを上げても、ぼやけはあまり変わりません。
- **ホストを終了する**: タスクトレイのアイコンを右クリックして「終了」を選ぶか、「情報」のタブの「WN-VRBridge を終了する」を押します。終了すると、Quest からつないでも SteamVR は自動で起動しません。

## 困ったとき
- **Quest に「PC を探しています」のまま**: PC と Quest が同じネットワークにいるか、ホストが起動しているか、ファイアウォールの許可ルールを登録したかを確かめてください。自動で見つからないときは、PC の IPv4 アドレスを書いた `host.txt` を Quest の `/sdcard/Android/data/com.woodennut27.wnvrbridge/files/` に置くと、そのアドレスにつなぎます（例: `adb push host.txt /sdcard/Android/data/com.woodennut27.wnvrbridge/files/host.txt`。中身は `192.168.1.10` のように 1 行）。
- **映像が乱れる・止まる**: 「設定」のタブでビットレートを下げてください。PC はアクセスポイントに有線でつなぎ、Quest はアクセスポイントの近くで使ってください。
- **ログ**: ホストのログは `%LOCALAPPDATA%\WN-VRBridge\logs\`（「情報」のタブの「ログのフォルダを開く」で開けます）、ドライバのログは SteamVR のログ（`Steam\logs\vrserver.txt`、`[WN-VRBridge]` の行）にあります。
- **フォルダを動かしたとき**: 「セットアップ」のタブに「別の場所にある WN-VRBridge のドライバも登録されています」「登録し直しが必要」と出ます。「別の場所の登録を解除する」を押してから、ドライバと許可ルールを登録し直してください。
- **やめるとき**: 「セットアップ」のタブで「登録を解除する」と「許可ルールを削除する」を押してから、フォルダを消してください。Quest のアプリは Quest の設定から削除できます。

## 細かい設定（上級者向け）
SteamVR の設定ファイル（`Steam\config\steamvr.vrsettings`）の `driver_wnvrbridge` に書くと、ドライバの動きを変えられます。SteamVR を止めてから書き換え、起動し直すと反映されます。既定値は `driver/wnvrbridge/resources/settings/default.vrsettings` にあります。foveation の「使う / 使わない」と比率、マイク、自動終了は、ホストが動いていればホストの設定が優先されます。

| キー | 既定値 | 内容 |
|---|---|---|
| `foveatedEncoding` | `true` | 画像の中心だけを細かく、まわりを粗く送る（ビットレートとデコードの時間が減る） |
| `foveationCenterWidth` / `foveationCenterHeight` | `0.45` / `0.40` | 細かく送る中心の大きさ（目の画像の幅・高さに対する割合） |
| `foveationCenterExtendUp` | `0.10` | 中心の区間を上側にだけ広げる量 |
| `foveationEdgeRatioX` / `foveationEdgeRatioY` | `4.0` / `5.0` | まわりを縮める比（ホストが動いているときは、ホストの「周りを縮める比率」を縦横の両方に使う） |
| `pacingRatio` | `0.0` | 1 フレームを送る時間を分散する割合（0 は分散しない。Wi-Fi が混んでいて映像が乱れるときに 0.5 などにする） |
| `controllerPitchOffsetDeg` ほか | — | コントローラの 3D モデルの位置と傾きの補正 |

---

## 開発者向け
要件は [docs/requirements.md](docs/requirements.md) にあります。

### ビルド
- PC: Visual Studio 2022（C++ のデスクトップ開発）。リポジトリのルートで:
  ```
  cmake --preset windows-x64
  cmake --build --preset release
  ctest --preset release
  ```
  ドライバは `build/windows-x64/driver/wnvrbridge/`、ホストは `build/windows-x64/host/` に出ます。開発中は `scripts/register-driver.ps1` / `scripts/unregister-driver.ps1` でドライバを登録・解除できます。
  `ctest` は 6 つの単体テストを実行します（Quest のコードのうち、PC で動かせる部分のテストを含みます）。
- Quest: Android Studio（JDK と Android SDK、NDK）。`quest/local.properties` に SDK の場所を書いてから:
  ```
  cd quest
  .\gradlew.bat assembleDebug
  ```

### 配布物を作る
```
pwsh scripts/package.ps1
```
PC と Quest をビルドし、`dist/WN-VRBridge-<版>/` と `dist/WN-VRBridge-<版>.zip` を作ります。版は、ルートの `VERSION` ファイルに書いてあります（ホスト、ドライバ、Quest のアプリのすべてがこの値を使います）。

### ホストの画面を確かめる
SteamVR と Quest を動かさなくても、ホストの画面を確かめられます。
```
pwsh scripts/fake-driver.ps1 -Seconds 30
```
を実行してからホストを起動すると、ドライバの代わりに統計が送られ、「Quest とつながっています」の表示になります。
```
pwsh scripts/capture-host-window.ps1 -Out shot.png -Page settings
```
は、指定したタブでホストを起動し、画面を画像に保存します。

## ライセンス
MIT License（[LICENSE](LICENSE)）。`third_party/` の外部ライブラリは、それぞれのライセンスに従います（OpenVR: BSD-3-Clause、Dear ImGui・doctest・nlohmann/json: MIT）。
