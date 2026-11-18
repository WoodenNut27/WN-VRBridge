#Requires -Version 7.0
param(
    [string]$Version,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $PSBoundParameters.ContainsKey('Version')) {
    # 版はルートの VERSION ファイルの 1 行目だけに書いてある（CMake と Gradle も同じファイルを読む）。
    $versionPath = Join-Path $repoRoot 'VERSION'
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw 'VERSION ファイルが見つかりません。'
    }
    $Version = (Get-Content -LiteralPath $versionPath -TotalCount 1 -Encoding UTF8).Trim()
}

# 版をパスの一部に使うため、別のフォルダを指す文字や Windows で使えない名前を拒否する。
if ($Version -notmatch '\A[0-9A-Za-z][0-9A-Za-z._+\-]*\z' -or $Version.EndsWith('.')) {
    throw '版には英数字と . _ + - を使用し、先頭は英数字、末尾は . 以外にしてください。'
}

if (-not $SkipBuild) {
    $cmakeCommand = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        $cmakePath = $cmakeCommand.Source
        $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    } else {
        $cmakeDirectory = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
        $cmakePath = Join-Path $cmakeDirectory 'cmake.exe'
        $ctestPath = Join-Path $cmakeDirectory 'ctest.exe'
    }
    foreach ($toolPath in @($cmakePath, $ctestPath)) {
        if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
            throw "ビルド用のツールが見つかりません: $toolPath"
        }
    }

    Push-Location -LiteralPath $repoRoot
    try {
        Write-Host 'PC のビルド設定を作成しています。'
        & $cmakePath --preset windows-x64
        if ($LASTEXITCODE -ne 0) {
            throw "PC のビルド設定に失敗しました（終了コード: $LASTEXITCODE）。"
        }
        Write-Host 'PC をビルドしています。'
        & $cmakePath --build --preset release
        if ($LASTEXITCODE -ne 0) {
            throw "PC のビルドに失敗しました（終了コード: $LASTEXITCODE）。"
        }
        Write-Host 'PC のテストを実行しています。'
        & $ctestPath --preset release
        if ($LASTEXITCODE -ne 0) {
            throw "PC のテストに失敗しました（終了コード: $LASTEXITCODE）。"
        }
    } finally {
        Pop-Location
    }

    if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
        $env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
    }
    Push-Location -LiteralPath (Join-Path $repoRoot 'quest')
    try {
        Write-Host 'Quest をビルドしています。'
        & './gradlew.bat' assembleDebug
        if ($LASTEXITCODE -ne 0) {
            throw "Quest のビルドに失敗しました（終了コード: $LASTEXITCODE）。"
        }
    } finally {
        Pop-Location
    }
}

$packageName = "WN-VRBridge-$Version"
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
$packageRoot = [System.IO.Path]::GetFullPath((Join-Path $distRoot $packageName))
$zipPath = Join-Path $distRoot "$packageName.zip"
$hostSource = Join-Path $repoRoot 'build/windows-x64/host/wnvrbridge_host.exe'
$driverSource = Join-Path $repoRoot 'build/windows-x64/driver/wnvrbridge'
$apkSource = Join-Path $repoRoot 'quest/app/build/outputs/apk/debug/app-debug.apk'
$readmeSource = Join-Path $repoRoot 'README.md'
$requiredDriverFiles = @(
    'bin/win64/driver_wnvrbridge.dll'
    'driver.vrdrivermanifest'
    'resources/settings/default.vrsettings'
)

# 不完全なビルド結果で、前回の配布フォルダを消してしまわないようにする。
foreach ($sourcePath in @($hostSource, $apkSource) + @($requiredDriverFiles | ForEach-Object {
    Join-Path $driverSource $_
})) {
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "配布に必要なビルド結果が見つかりません: $sourcePath"
    }
}

# 再帰削除の前に、対象がリポジトリ内の dist 直下であることと、リンクでないことを確認する。
if ((Split-Path -Parent $packageRoot) -ne $distRoot) {
    throw "配布フォルダが dist の外を指しています: $packageRoot"
}
foreach ($targetPath in @($distRoot, $packageRoot)) {
    if ((Test-Path -LiteralPath $targetPath) -and
        ((Get-Item -LiteralPath $targetPath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "配布先にリンクは使用できません: $targetPath"
    }
}
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'quest') -Force | Out-Null
Copy-Item -LiteralPath $hostSource -Destination (Join-Path $packageRoot 'wnvrbridge_host.exe')
Copy-Item -LiteralPath $apkSource -Destination (Join-Path $packageRoot "quest/$packageName.apk")

$driverDestination = Join-Path $packageRoot 'driver/wnvrbridge'
New-Item -ItemType Directory -Path $driverDestination -Force | Out-Null
foreach ($sourceFile in Get-ChildItem -LiteralPath $driverSource -Recurse -File -Force) {
    if ($sourceFile.Extension -ieq '.pdb') {
        continue
    }
    $relativePath = [System.IO.Path]::GetRelativePath($driverSource, $sourceFile.FullName)
    $destination = Join-Path $driverDestination $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile.FullName -Destination $destination
}
$hasReadme = Test-Path -LiteralPath $readmeSource -PathType Leaf
if ($hasReadme) {
    Copy-Item -LiteralPath $readmeSource -Destination (Join-Path $packageRoot 'README.md')
} else {
    Write-Warning 'README.md がまだ無いため、README なしで配布物を作成します。'
}

$requiredFiles = @('wnvrbridge_host.exe', "quest/$packageName.apk") + @(
    $requiredDriverFiles | ForEach-Object { "driver/wnvrbridge/$_" }
)
if ($hasReadme) {
    $requiredFiles += 'README.md'
}
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $relativePath) -PathType Leaf)) {
        throw "配布物に必要なファイルがありません: $relativePath"
    }
}
foreach ($relativePath in @('wnvrbridge_host.exe', 'driver/wnvrbridge/bin/win64/driver_wnvrbridge.dll')) {
    $binaryPath = Join-Path $packageRoot $relativePath
    $binaryText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($binaryPath))
    if ($binaryText -match 'VCRUNTIME140|MSVCP140') {
        throw "VC++ の実行時 DLL への依存が残っています。静的リンクでビルドし直してください: $relativePath"
    }
}

Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -Force
$zipFile = Get-Item -LiteralPath $zipPath
Write-Host "配布 zip を作成しました: $($zipFile.FullName)"
Write-Host ("サイズ: {0:N0} バイト（{1:N2} MiB）" -f $zipFile.Length, ($zipFile.Length / 1MB))
Write-Host 'zip 内のファイル一覧:'
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($entry in $archive.Entries | Sort-Object FullName) {
        Write-Host ("  {0}（{1:N0} バイト）" -f $entry.FullName, $entry.Length)
    }
} finally {
    $archive.Dispose()
}
