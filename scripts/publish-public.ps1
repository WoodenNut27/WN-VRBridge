#Requires -Version 7.0
# 非公開の開発用リポジトリの内容から、開発の記録などを除いたものを公開用のリポジトリに写して 1 つのコミットにする。
# 公開用には開発用の履歴を持ち込まない（作者のメールアドレスや、開発の記録に書いた家の LAN の情報を出さないため）。
param(
    [Parameter(Mandatory)][string]$Message,
    # 公開用のリポジトリを置く手元のフォルダ。無ければ作る。
    [string]$PublicPath = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'WN-VRBridge-public'),
    [string]$RemoteUrl = 'https://github.com/WoodenNut27/WN-VRBridge.git',
    [switch]$Push
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# 公開用のコミットの作者。GitHub の公開用のアドレスを使い、個人のメールアドレスを出さない。
$authorName = 'WoodenNut27'
$authorEmail = '100458993+WoodenNut27@users.noreply.github.com'

# 公開しないもの（リポジトリのルートからの相対パス）。
$excluded = @(
    'AGENTS.md'
    'CLAUDE.md'
    'docs/progress.md'
    'docs/next-codex-task.md'
    'docs/refactoring-2026-11.md'
    'docs/feasibility-study.md'
    'docs/research-oss.md'
    'docs/images'
)

# Markdown のうち、この印で囲んだ部分は公開用から取り除く（公開しない資料へのリンクなど）。
$devOnlyPattern = '(?s)<!-- dev-only -->.*?<!-- /dev-only -->\r?\n?'

function Invoke-Git {
    param([string]$Directory, [string[]]$Arguments)
    & git -C $Directory @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') に失敗しました（$Directory）"
    }
}

# 写すのはコミット済みの内容だけ。作業中の変更が混ざらないよう、残っていれば知らせる。
$dirty = & git -C $repoRoot status --porcelain --untracked-files=no
if ($dirty) {
    Write-Warning 'コミットしていない変更があります。公開用には、コミット済みの内容（HEAD）だけを写します。'
}
$sourceCommit = (& git -C $repoRoot rev-parse --short HEAD).Trim()

$staging = Join-Path ([System.IO.Path]::GetTempPath()) "wnvr-public-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    $archive = Join-Path $staging 'source.tar'
    Invoke-Git $repoRoot @('archive', '--format=tar', "--output=$archive", 'HEAD')
    $tree = Join-Path $staging 'tree'
    New-Item -ItemType Directory -Path $tree | Out-Null
    & tar -xf $archive -C $tree
    if ($LASTEXITCODE -ne 0) {
        throw 'アーカイブを展開できませんでした。'
    }

    foreach ($path in $excluded) {
        $target = Join-Path $tree $path
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force
        }
    }

    foreach ($file in Get-ChildItem -LiteralPath $tree -Recurse -File -Filter '*.md') {
        $text = [System.IO.File]::ReadAllText($file.FullName)
        $stripped = [regex]::Replace($text, $devOnlyPattern, '')
        if ($stripped -ne $text) {
            [System.IO.File]::WriteAllText($file.FullName, $stripped, [System.Text.UTF8Encoding]::new($false))
        }
        if ($stripped -match '<!-- /?dev-only -->') {
            throw "dev-only の印が対になっていません: $($file.FullName.Substring($tree.Length + 1))"
        }
    }

    # 除いた資料へのリンクが残っていると、公開用で開けないリンクになる。
    $names = $excluded | ForEach-Object { Split-Path -Leaf $_ }
    foreach ($file in Get-ChildItem -LiteralPath $tree -Recurse -File -Filter '*.md') {
        $text = [System.IO.File]::ReadAllText($file.FullName)
        foreach ($name in $names) {
            if ($text.Contains("]($name)") -or $text.Contains("/$name)")) {
                throw "公開しない資料へのリンクが残っています: $($file.FullName.Substring($tree.Length + 1)) → $name"
            }
        }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $PublicPath '.git'))) {
        New-Item -ItemType Directory -Path $PublicPath -Force | Out-Null
        $remoteHasMain = $false
        & git ls-remote --exit-code --heads $RemoteUrl main *> $null
        if ($LASTEXITCODE -eq 0) {
            $remoteHasMain = $true
        }
        if ($remoteHasMain) {
            Invoke-Git (Split-Path -Parent $PublicPath) @('clone', $RemoteUrl, (Split-Path -Leaf $PublicPath))
        } else {
            Invoke-Git $PublicPath @('init', '-b', 'main')
            Invoke-Git $PublicPath @('remote', 'add', 'origin', $RemoteUrl)
        }
    }

    # GitHub は改名したリポジトリの元の名前を新しい名前へ転送するので、公開用のリポジトリが無いと
    # 開発用のリポジトリを clone してしまうことがある。公開用のアドレス以外の作者のコミットがあれば止める。
    & git -C $PublicPath rev-parse --verify -q HEAD *> $null
    if ($LASTEXITCODE -eq 0) {
        $authors = & git -C $PublicPath log --format='%ae' | Sort-Object -Unique
        $foreign = @($authors | Where-Object { $_ -ne $authorEmail })
        if ($foreign.Count -ne 0) {
            throw "公開用のリポジトリ（$PublicPath）に、公開用のアドレス以外の作者のコミットがあります。開発用のリポジトリを指していないか確かめてください: $($foreign -join ', ')"
        }
    }

    # 公開用の中身を、今の開発用の内容でそっくり置き換える（消えたファイルも反映する）。
    Get-ChildItem -LiteralPath $PublicPath -Force | Where-Object Name -ne '.git' | Remove-Item -Recurse -Force
    Copy-Item -Path (Join-Path $tree '*') -Destination $PublicPath -Recurse -Force
    Get-ChildItem -LiteralPath $tree -Force -Filter '.*' | Copy-Item -Destination $PublicPath -Recurse -Force

    Invoke-Git $PublicPath @('add', '-A')
    & git -C $PublicPath diff --cached --quiet
    if ($LASTEXITCODE -eq 0) {
        Write-Host '公開用に反映する変更はありません。'
    } else {
        Invoke-Git $PublicPath @('-c', "user.name=$authorName", '-c', "user.email=$authorEmail", 'commit', '-q', '-m', $Message, '-m', "開発用のコミット: $sourceCommit")
        Write-Host "公開用にコミットしました: $((& git -C $PublicPath log -1 --format='%h %s'))"
    }

    if ($Push) {
        Invoke-Git $PublicPath @('push', '-u', 'origin', 'main')
        Write-Host "公開用のリポジトリに push しました: $RemoteUrl"
    } else {
        Write-Host "まだ push していません。中身を確かめてから -Push を付けて実行するか、$PublicPath で git push してください。"
    }
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
