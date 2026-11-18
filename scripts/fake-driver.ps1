#Requires -Version 7.0
<#
.SYNOPSIS
    開発用: SteamVR のドライバの代わりに、ホストへ状態と統計を送る。
.DESCRIPTION
    SteamVR と Quest を動かさずに、ホストの設定画面の「SteamVR が起動している」「Quest とつながっている」
    ときの表示を確かめるために使う。ドライバと同じ名前付きパイプを作り、1 秒ごとに決まった値を送る。
    このスクリプトを先に起動してから、ホストを起動する（ホストは 1 秒ごとにパイプを探す）。
    止めると、ホストは「SteamVR が終了した」ときと同じ動きをする。
    本物のドライバと同じパイプを使うので、SteamVR が動いているあいだは使えない。
.EXAMPLE
    # Quest とつながっている表示を 30 秒間。
    pwsh scripts/fake-driver.ps1 -Seconds 30
.EXAMPLE
    # SteamVR だけが動いていて、Quest はつながっていない表示。
    pwsh scripts/fake-driver.ps1 -NoClient
.EXAMPLE
    # パケットの損失が多いときの色を確かめる。
    pwsh scripts/fake-driver.ps1 -LossPercent 6.5
#>
param(
    [int]$Seconds = 20,
    [switch]$NoClient,
    [double]$LossPercent = 0.12
)

$ErrorActionPreference = 'Stop'
if (Get-Process vrserver -ErrorAction SilentlyContinue) {
    throw 'SteamVR が動いています。本物のドライバとパイプがぶつかるので、SteamVR を終了してから実行してください。'
}

# 名前とメッセージの形は shared/host_ipc/include/wnvr/host_ipc/host_ipc.h と合わせる。
$pipe = [System.IO.Pipes.NamedPipeServerStream]::new('WN-VRBridge.Driver', [System.IO.Pipes.PipeDirection]::InOut, 1,
    [System.IO.Pipes.PipeTransmissionMode]::Message, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536)
try {
    Write-Host 'ホストの接続を待っています（ホストを起動してください）。'
    $pipe.WaitForConnection()
    Write-Host "ホストがつながりました。$Seconds 秒間、統計を送ります。"

    function Send-Message($message) {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($message | ConvertTo-Json -Compress -Depth 5))
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
    }

    Send-Message @{ type = 'hello'; protocolVersion = 2; version = 'fake' }
    $connected = -not $NoClient
    # 項目は StatsSnapshot と同じ。1 つでも欠けると、ホストはメッセージを受け付けない。
    $stats = [ordered]@{
        clientConnected = $connected
        clientName = $connected ? 'Meta Quest 3' : ''
        clientSessionId = 12345
        renderFps = 71.9; encodeFps = 71.9; encodeAverageMs = 2.8; encodeMaxMs = 4.4
        sentMbps = 76.02; sentFragments = 51234; sendDroppedFrames = 0; virtualVsyncHz = 71.94
        receivedFps = 71.9; lostFrames = 0; fragmentLossPercent = $LossPercent; receivedMbps = 75.32
        decodeAverageMs = 11.2; decodeMaxMs = 24.3; displayFps = 72.1; repeatedFrames = 5
        latencyAverageMs = 47.3; latencyMaxMs = 58.6; roundTripMs = 3.1
        audioUnderruns = 0; audioMissingFrames = 0; audioBufferAverageMs = 34.7
    }
    $end = (Get-Date).AddSeconds($Seconds)
    try {
        while ((Get-Date) -lt $end -and $pipe.IsConnected) {
            Send-Message @{ type = 'status'; stats = $stats }
            Start-Sleep -Milliseconds 900
        }
    } catch [System.IO.IOException] {
        # ホストが先に終了すると、書き込みが失敗する。確認用のスクリプトなので、そのまま終わる。
        Write-Host 'ホストがパイプを閉じました。'
    }
} finally {
    $pipe.Dispose()
}
