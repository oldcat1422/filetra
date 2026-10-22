# FileTra · 局域网文件传输（Go / C 双版本）

一个**单文件、零依赖、开箱即用**的局域网文件传输工具。双击运行，同一 Wi-Fi 下的手机、平板、其他电脑打开浏览器即可互传文件和文本。

仓库提供两个**功能完全一致**的实现，面向不同目标平台：

| 目录 | 版本 | 产物体积 | 主要适配 |
|------|------|----------|----------|
| `filetra-go/` | Go（go:embed 内嵌前端） | 约 7 MB | Windows 10+ / Linux / macOS，一次构建 8 个平台 |
| `filetra-c/`  | C（手写 HTTP + Winsock2） | 约 360 KB | **仅 Windows**：XP SP3（32 位）～ Windows 11（经 WoW64 同时覆盖 64 位） |

> 💡 选型建议：现代系统 / 需要 Linux、macOS → **Go 版**；老 Windows（XP SP3 / Win7 / Win8 / 32 位老机器）或追求极致单文件绿色体积 → **C 版**。Go 版最低也要 Windows 10（见下文兼容矩阵），**XP/7/8 老机器请用 C 版**。

## ✨ 功能（两版一致）

| 功能 | 说明 |
|------|------|
| 📂 文件浏览 | 网页直接浏览共享目录，支持子目录、筛选、排序、新建文件夹 |
| ⬇️ 下载 | 单文件下载（支持 HTTP Range **断点续传**）；在线预览图片/文本/音视频 |
| 🗜️ 打包下载 | 整个文件夹流式打包 zip 下载，不占内存不落盘 |
| ⬆️ 上传 | 拖拽或选择文件/整个文件夹上传；**4MB 分片 + 服务端校验，中途断网自动续传** |
| 💬 文本速递 | 设备间传剪贴板文本/临时便签，全端自动同步，持久保存最近 100 条 |
| 📡 附近设备 | UDP 广播自动发现同网段运行的 FileTra，一键跳转对方共享目录 |
| 📱 扫码连接 | 自动生成二维码，手机扫码打开、扫码上传 |
| 🕘 传输记录 | 收发历史自动记录并持久化（最多 500 条） |
| 🔒 访问密码 | 可选 `-password` 开启 Basic Auth 保护 |

## 📂 仓库结构

```
filetra-go/          Go 版源码与构建脚本
├── main.go / api.go / upload.go / discovery.go / store.go / util.go / config.go
├── go.mod / go.sum
├── build.ps1        Windows 一键交叉构建（8 平台 → dist\）
├── build.sh         Linux/macOS 一键交叉构建
├── README.md        Go 版详细文档
└── web/             共享前端（原生 JS/CSS，无第三方依赖）

filetra-c/           C 版源码与构建脚本（仅 Windows）
├── build-c.ps1      zig cc 一键编译 → cdist\filetra-c.exe
├── csrc/
│   ├── main.c       服务端主体（手写 HTTP，无任何运行时依赖）
│   ├── qrcodegen.c  二维码生成（nayuki/QR-Code-generator）
│   └── assets.h     内嵌前端字节（由 tools\gen-assets.mjs 生成，已随仓库提交）
├── tools/
│   ├── gen-assets.mjs     把 web/ 生成 assets.h（需 node，可选）
│   ├── fetch-assets.mjs   下载 zig 工具链 + qrcodegen 源码（可选）
│   └── smoke-c.ps1        冒烟测试（30 项断言）
├── web/             共享前端（与 filetra-go\web\ 内容一致）
└── zig-windows-x86_64-0.13.0\   zig 编译器（未入库，见"构建"获取）
```

> `web/` 同时被两端引用：Go 版用 `go:embed` 直接内嵌；C 版由 `gen-assets.mjs` 转成 `assets.h` 编译进 exe。因此两个目录各存了一份，**修改时保持两边一致**。

## 🚀 快速开始

### 启动任一版本

```powershell
# 默认：共享 exe 所在目录，端口 5050，自动打开浏览器
.\filetra.exe            # Go 版（build.ps1 产物）
.\filetra-c.exe          # C 版（build-c.ps1 产物）

# 共享指定目录
.\filetra.exe -dir D:\电影

# 带访问密码（登录用户名默认 filetra，可用 -user 自定义）
.\filetra.exe -dir D:\共享 -password 我的密码
.\filetra.exe -dir D:\共享 -user 老猫 -password 我的密码
```

两版参数**完全一致**（均支持 `-flag value` 与 `-flag=value`；C 版还接受 `--flag` 长横线写法）：

| 参数 | 默认 | 说明 |
|------|------|------|
| `-dir` | 当前目录 | 要共享的目录 |
| `-port` | 5050 | HTTP 端口，被占用时自动顺延 |
| `-name` | 计算机名 | 设备显示名称 |
| `-password` | 无 | 访问密码（Basic Auth） |
| `-user` | filetra | 登录用户名（不能包含冒号 `:`） |
| `-no-browser` | false | 启动后不自动打开浏览器 |

### ⚙️ 外置配置文件 filetra.conf

两版均支持与 exe **同目录**的 `filetra.conf`。**文件不存在时行为完全不变**（内置默认值 + 命令行参数）。

- 优先级：**配置文件 > 命令行参数 > 内置默认值**——命令行仅在配置文件未提供对应键时生效
- 格式：UTF-8 文本（可带 BOM），`key=value`，`#` 开头整行注释；空值=未设置；未知键/不合法值打印提示后忽略
- `dir` 相对路径按启动时的工作目录解析（与 `-dir` 行为一致）

```conf
# FileTra 配置（修改后重启生效）
port=5050
name=书房电脑
dir=D:\电影
password=
user=filetra
no-browser=false
```

| 键 | 默认 | 说明 |
|----|------|------|
| `port` | 5050 | HTTP 端口（1~65535，被占用时自动顺延） |
| `name` | 计算机名 | 设备显示名称 |
| `dir` | 当前目录 | 共享目录——**客户端只能浏览/操作该目录内的文件**（含 `..` 穿越防护） |
| `password` | 无 | 访问密码（Basic Auth）；留空=不启用 |
| `user` | filetra | 登录用户名，不能包含冒号 `:` |
| `no-browser` | false | 可用 true/false、1/0、yes/no、on/off（不区分大小写） |

启动后会打印访问地址（如 `http://192.168.1.10:5050`），其他设备：

1. 连接**同一个 Wi-Fi / 网段**
2. 浏览器访问该地址，或直接用手机相机扫"设备"页的二维码
3. 即可上传、下载、互传文本

> 首次运行 Windows 可能弹出防火墙询问，请选择 **允许**（涉及 TCP 服务端口与 UDP 52020~52027 广播）。客户端浏览器无任何系统要求，能开现代浏览器即可。

## 🛠 构建方式

### Go 版（需 Go 1.24+）

```powershell
cd filetra-go
.\build.ps1     # Windows；产出 dist\ 下 8 个平台单文件
./build.sh      # Linux/macOS
```

手动构建本机版本：

```bash
go build -trimpath -ldflags "-s -w" -o filetra.exe .
```

### C 版（仅需仓库自带的 zig，产物见 cdist\）

`csrc\assets.h` 已随仓库提交，克隆后**开箱即可构建**：

```powershell
cd filetra-c
.\build-c.ps1          # 产出 cdist\filetra-c.exe（i386 静态，XP SP3+ 可运行）
```

如本地没有 zig 工具链，二选一：

```powershell
# 方式一：node 自动下载（放入 tools\）
cd filetra-c; node tools\fetch-assets.mjs

# 方式二：手动下载解压
# 下载 https://ziglang.org/download/0.13.0/zig-windows-x86_64-0.13.0.zip
# 解压到 filetra-c\tools\zig-windows-x86_64-0.13.0\
```

修改了 `web/` 前端后重新生成内嵌资源（需要 node）：

```bash
cd filetra-c && node tools/gen-assets.mjs   # 重新生成 csrc/assets.h
# 同时把 web/ 修改同步到 filetra-go\web\，Go 版重新 build 即可生效
```

测试：

```powershell
# 先启动任一实例（如 .\cdist\filetra-c.exe -port 5090 -no-browser），再：
powershell -File tools\smoke-c.ps1 -Port 5090     # 30 项断言
```

## 🧩 各产物适配的系统版本

### Go 版（用 Go 1.24 构建，最低系统要求如下）

| 产物（dist\ 内） | 目标架构 | 适配系统 |
|------------------|----------|----------|
| `filetra-windows-amd64.exe` | x64 | Windows 10 / Windows Server 2016 及更新（64 位） |
| `filetra-windows-arm64.exe` | ARM64 | Windows 10/11 ARM 设备（如 Surface Pro X） |
| `filetra-windows-386.exe` | x86（32 位） | 32 位 Windows 10（注意：仍要求 Win10，**不支持 XP/7/8**） |
| `filetra-linux-amd64` | x64 | Linux 内核 2.6.32+，64 位（静态编译，无 glibc 依赖，Alpine 等可直接运行） |
| `filetra-linux-arm64` | ARM64 | 同上（树莓派 3/4/5、ARM 服务器等） |
| `filetra-linux-386` | x86（32 位） | 同上（32 位老设备） |
| `filetra-darwin-amd64` | x64 | Intel Mac，macOS 10.15 Catalina 及更新 |
| `filetra-darwin-arm64` | ARM64 | Apple Silicon（M1 起），macOS 11 及更新 |

### C 版

| 产物 | 目标架构 | 适配系统 |
|------|----------|----------|
| `cdist\filetra-c.exe` | x86（i386 静态，无 CRT 依赖） | **Windows XP SP3 ～ Windows 11**：32 位系统原生运行，64 位系统经 WoW64 运行 |

> 老机器底线：CPU 需支持 SSE2 指令（约 2004 年后的 x86 CPU 均支持，覆盖 XP SP3 时代的主流硬件）。

### 网页客户端（两端通用）

运行现代浏览器的任意设备（手机 / 平板 / 电脑均可），无操作系统版本要求。

## 📡 端口与网络

| 端口 | 协议 | 用途 |
|------|------|------|
| 5050 起 | TCP | HTTP 服务（被占用自动顺延） |
| 52020~52027 | UDP | 附近设备广播发现（同机多开不冲突） |

## 📝 两版已知实现差异

- C 版 zip 打包使用 STORE（不压缩）方式，速度更快、兼容老解压器；
- C 版二维码 PNG 未压缩（约 100 KB，首次加载略慢），Go 版约 3 KB；
- 全响应均带 `Cache-Control: no-cache`，页面数据始终实时。

Go 版更多细节见 [`filetra-go/README.md`](filetra-go/README.md)。
