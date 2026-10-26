# FileTra · 局域网文件传输

一个**单文件、零依赖、开箱即用**的局域网文件传输工具。双击运行，同一 Wi-Fi 下的手机、平板、其他电脑打开浏览器即可互传文件和文本。

纯粹服务于 **Windows**（XP SP3 32 位 ～ Windows 11 64 位通用），单文件约 360 KB，无需安装任何运行时。

## ✨ 功能

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
filetra-c/            C 版源码与构建脚本（仅 Windows）
├── build-c.ps1      zig cc 一键编译 → cdist\filetra-c.exe
├── csrc/
│   ├── main.c       服务端主体（手写 HTTP，无任何运行时依赖）
│   ├── qrcodegen.c  二维码生成（nayuki/QR-Code-generator）
│   └── assets.h     内嵌前端字节（由 tools\gen-assets.mjs 生成，已随仓库提交）
├── tools/
│   ├── gen-assets.mjs     把 web/ 生成 assets.h（需 node，可选）
│   ├── fetch-assets.mjs   下载 zig 工具链 + qrcodegen 源码（可选）
│   └── smoke-c.ps1        冒烟测试（30 项断言）
└── web/             共享前端（原生 JS/CSS，无第三方依赖）

zig 提示：本地如无 zig 工具链则不入库，见下文"构建"获取。
```

> `web/` 由 `tools\gen-assets.mjs` 转成 `csrc/assets.h` 编译进 exe；修改前端后重新生成 assets.h 即可。

## 🚀 快速开始

### 启动

```powershell
# 默认：共享 exe 所在目录，端口 5050，自动打开浏览器
.\filetra-c.exe          # C 版（build-c.ps1 产物）

# 共享指定目录
.\filetra-c.exe -dir D:\电影

# 带访问密码（登录用户名默认 filetra，可用 -user 自定义）
.\filetra-c.exe -dir D:\共享 -password 我的密码
.\filetra-c.exe -dir D:\共享 -user 老猫 -password 我的密码
```

参数（支持 `-flag value` 与 `-flag=value`，也接受 `--flag` 长横线写法）：

| 参数 | 默认 | 说明 |
|------|------|------|
| `-dir` | 当前目录 | 要共享的目录 |
| `-port` | 5050 | HTTP 端口，被占用时自动顺延 |
| `-name` | 计算机名 | 设备显示名称 |
| `-password` | 无 | 访问密码（Basic Auth） |
| `-user` | filetra | 登录用户名（不能包含冒号 `:`） |
| `-no-browser` | false | 启动后不自动打开浏览器 |

### ⚙️ 外置配置文件 filetra.conf

支持与 exe **同目录**的 `filetra.conf`。**文件不存在时行为完全不变**（内置默认值 + 命令行参数）。

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
```

测试：

```powershell
# 先启动任一实例（如 .\cdist\filetra-c.exe -port 5090 -no-browser），再：
powershell -File tools\smoke-c.ps1 -Port 5090     # 30 项断言
```

## 🧩 产物适配的系统版本

### C 版（filetra-c.exe）

| 产物 | 目标架构 | 适配系统 |
|------|----------|----------|
| `cdist\filetra-c.exe` | x86（i386 静态，无 CRT 依赖） | **Windows XP SP3 ～ Windows 11**：32 位系统原生运行，64 位系统经 WoW64 运行 |

> 老机器底线：CPU 需支持 SSE2 指令（约 2004 年后的 x86 CPU 均支持，覆盖 XP SP3 时代的主流硬件）。

### 网页客户端（通用）

运行现代浏览器的任意设备（手机 / 平板 / 电脑均可），无操作系统版本要求。

## 📡 端口与网络

| 端口 | 协议 | 用途 |
|------|------|------|
| 5050 起 | TCP | HTTP 服务（被占用自动顺延） |
| 52020~52027 | UDP | 附近设备广播发现（同机多开不冲突） |

## 📝 已知实现说明

- zip 打包使用 STORE（不压缩）方式，速度更快、兼容老解压器；
- 二维码 PNG 未压缩（约 100 KB，首次加载略慢）；
- 全响应均带 `Cache-Control: no-cache`，页面数据始终实时。
