# FileTra 构建脚本：一键产出多平台单文件可执行程序
# 用法：右键"使用 PowerShell 运行"，或在终端执行 .\build.ps1

$ErrorActionPreference = "Stop"
$env:CGO_ENABLED = "0"

# 自动定位 go（新装工具链可能还没进当前会话 PATH）
$goCmd = Get-Command go -ErrorAction SilentlyContinue
if (-not $goCmd) {
	$candidates = @("C:\Program Files\Go\bin\go.exe", "$env:LOCALAPPDATA\Programs\Go\bin\go.exe")
	foreach ($c in $candidates) {
		if (Test-Path $c) { $goCmd = $c; break }
	}
}
if (-not $goCmd) { throw "未找到 go.exe，请先安装 Go 1.24+ 并加入 PATH" }
$go = if ($goCmd.Source) { $goCmd.Source } else { "$goCmd" }

$targets = @(
	@{ GOOS = "windows"; GOARCH = "amd64"; EXT = ".exe" },
	@{ GOOS = "windows"; GOARCH = "arm64"; EXT = ".exe" },
	@{ GOOS = "windows"; GOARCH = "386";   EXT = ".exe" },
	@{ GOOS = "linux";   GOARCH = "amd64"; EXT = "" },
	@{ GOOS = "linux";   GOARCH = "arm64"; EXT = "" },
	@{ GOOS = "linux";   GOARCH = "386";   EXT = "" },
	@{ GOOS = "darwin";  GOARCH = "amd64"; EXT = "" },
	@{ GOOS = "darwin";  GOARCH = "arm64"; EXT = "" }
)

New-Item -ItemType Directory -Force -Path dist | Out-Null

foreach ($t in $targets) {
	$env:GOOS = $t.GOOS
	$env:GOARCH = $t.GOARCH
	$out = "dist/filetra-$($t.GOOS)-$($t.GOARCH)$($t.EXT)"
	Write-Host "building $out ..." -ForegroundColor Cyan
	& $go build -trimpath -ldflags "-s -w" -o $out .
	if ($LASTEXITCODE -ne 0) { throw "build failed: $out" }
}

Write-Host "`n全部构建完成，见 dist\ 目录" -ForegroundColor Green
Get-ChildItem dist | Format-Table Name, @{n="SizeMB"; e={[math]::Round($_.Length/1MB, 2)}}