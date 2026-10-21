#!/usr/bin/env bash
# FileTra 构建脚本（Linux/macOS）：一键产出多平台单文件可执行程序
set -euo pipefail

export CGO_ENABLED=0

targets=(
	"windows amd64 .exe"
	"windows arm64 .exe"
	"windows 386 .exe"
	"linux amd64"
	"linux arm64"
	"linux 386"
	"darwin amd64"
	"darwin arm64"
)

if ! command -v go >/dev/null 2>&1; then
	echo "未找到 go，请先安装 Go 1.24+" >&2
	exit 1
fi

mkdir -p dist
for t in "${targets[@]}"; do
	read -r goos goarch ext <<<"$t"
	out="dist/filetra-${goos}-${goarch}${ext:-}"
	echo "building $out ..."
	GOOS="$goos" GOARCH="$goarch" go build -trimpath -ldflags "-s -w" -o "$out" .
done

echo
echo "全部构建完成，见 dist/ 目录"
ls -lh dist
