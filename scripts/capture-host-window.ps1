#Requires -Version 7.0
<#
.SYNOPSIS
    開発用: ホストの設定画面を画像（PNG）に保存する。
.DESCRIPTION
    画面の見た目を変えたあとに、実際の表示を確かめるために使う。
    -Page を付けると、ビルドしたホスト（build/windows-x64/host）をそのタブで起動してから撮り、撮ったあとに終了する。
    付けなければ、すでに開いている設定画面をそのまま撮る。
    ウィンドウがほかのウィンドウに隠れていても撮れる。
.EXAMPLE
    pwsh scripts/capture-host-window.ps1 -Out shot.png -Page settings
.EXAMPLE
    # 縦に長いタブを、スクロールせずに全部撮る。
    pwsh scripts/capture-host-window.ps1 -Out shot.png -Page setup -Width 880 -Height 1200
#>
param(
    [Parameter(Mandatory)][string]$Out,
    [ValidateSet('status', 'settings', 'setup', 'about')][string]$Page,
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$WaitSeconds = 3
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WnvrCapture {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string className, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$started = $null
if ($Page) {
    # 2 つ目を起動しても、動いている方の画面が前に出るだけでタブは変わらない。
    if (Get-Process wnvrbridge_host -ErrorAction SilentlyContinue) {
        throw 'ホストがすでに動いています。終了してから実行するか、-Page を付けずに実行してください。'
    }
    $hostPath = Join-Path $repoRoot 'build/windows-x64/host/wnvrbridge_host.exe'
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) {
        throw "ホストが見つかりません。先にビルドしてください: $hostPath"
    }
    $started = Start-Process -FilePath $hostPath -ArgumentList '--page', $Page -PassThru
    Start-Sleep -Seconds $WaitSeconds
}

try {
    # 題名は問わないので null を渡す（PowerShell の $null は空文字列になってしまう）。
    $window = [WnvrCapture]::FindWindowW('WNVRBridgeSettingsWindow', [NullString]::Value)
    if ($window -eq [IntPtr]::Zero) {
        throw '設定画面が見つかりません。ホストを起動して、設定画面を開いてください。'
    }
    if ($Width -gt 0 -and $Height -gt 0) {
        # 位置と重なりの順番は変えず、大きさだけを変える。
        [WnvrCapture]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, $Width, $Height, 0x0002 -bor 0x0004 -bor 0x0010) | Out-Null
        Start-Sleep -Milliseconds 400
    }
    $rect = New-Object WnvrCapture+RECT
    [WnvrCapture]::GetWindowRect($window, [ref]$rect) | Out-Null
    $bitmap = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        # 2 = PW_RENDERFULLCONTENT。Direct3D で描いた中身も取り込む。
        [WnvrCapture]::PrintWindow($window, $dc, 2) | Out-Null
        $graphics.ReleaseHdc($dc)
    } finally {
        $graphics.Dispose()
    }
    $outPath = [System.IO.Path]::GetFullPath($Out)
    $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "保存しました: $outPath（$($bitmap.Width) x $($bitmap.Height)）"
    $bitmap.Dispose()
} finally {
    if ($started -and -not $started.HasExited) {
        Stop-Process -Id $started.Id -Force
    }
}
