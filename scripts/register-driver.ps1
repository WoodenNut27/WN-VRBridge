#Requires -Version 7.0
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$driverPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build/windows-x64/driver/wnvrbridge'))
if (-not (Test-Path -LiteralPath (Join-Path $driverPath 'driver.vrdrivermanifest') -PathType Leaf)) {
    throw 'ドライバが見つかりません。先にビルドしてください。'
}

$pathsFile = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
if (-not (Test-Path -LiteralPath $pathsFile -PathType Leaf)) {
    throw 'SteamVR のパス設定が見つかりません。'
}
$paths = Get-Content -LiteralPath $pathsFile -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $paths.runtime -or [string]::IsNullOrWhiteSpace($paths.runtime[0])) {
    throw 'SteamVR のランタイムが見つかりません。'
}
$vrPathReg = Join-Path $paths.runtime[0] 'bin/win64/vrpathreg.exe'
if (-not (Test-Path -LiteralPath $vrPathReg -PathType Leaf)) {
    throw 'SteamVR の vrpathreg.exe が見つかりません。'
}
& $vrPathReg adddriver $driverPath
if ($LASTEXITCODE -ne 0) {
    throw "ドライバの登録に失敗しました（終了コード: $LASTEXITCODE）。"
}
Write-Host "ドライバを登録しました: $driverPath"
