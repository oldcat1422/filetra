# 构建 C 版 FileTra：i686 静态单 exe（XP SP3 ~ Win11 / 32、64 位通用）
# 前置：tools\ 下已解压 zig（zig-windows-x86_64-*），csrc\assets.h 已生成（node tools\gen-assets.mjs）
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$zigDir = Get-ChildItem (Join-Path $root 'tools\zig-windows-*') -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $zigDir){ Write-Host 'tools\ 下未找到 zig 工具链目录' -ForegroundColor Red; exit 1 }
$zig = Join-Path $zigDir.FullName 'zig.exe'

New-Item -ItemType Directory -Force (Join-Path $root 'cdist') | Out-Null

& $zig cc `
    -target x86-windows-gnu `
    -O2 -g0 `
    -U_WIN32_WINNT -D_WIN32_WINNT=0x0501 -UWINVER -DWINVER=0x0501 `
    -D__USE_MINGW_ANSI_STDIO=1 `
    (Join-Path $root 'csrc\main.c') (Join-Path $root 'csrc\qrcodegen.c') `
    -o (Join-Path $root 'cdist\filetra-c.exe') `
    -lws2_32 -liphlpapi -lshell32 -ladvapi32

if ($LASTEXITCODE -ne 0){ Write-Host "编译失败（exit $LASTEXITCODE）" -ForegroundColor Red; exit $LASTEXITCODE }
Write-Host "OK -> cdist\filetra-c.exe" -ForegroundColor Green
