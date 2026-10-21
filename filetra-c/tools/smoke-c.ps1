# C 版 FileTra 冒烟测试（需先构建 cdist\filetra-c.exe 并启动）
# 用法：.\tools\smoke-c.ps1 -Port 5060
# 所有 HTTP 响应经临时文件 + 显式 UTF-8 读取，避免控制台码页（GBK）解码破坏 JSON
param([int]$Port = 5060, [string]$Base = "http://127.0.0.1")

$ErrorActionPreference = 'Continue'
$u = "$Base`:$Port"
$PASS = 0; $FAIL = 0
function Check($name, $cond){
    if ($cond){ $script:PASS++; Write-Host "  [PASS] $name" }
    else { $script:FAIL++; Write-Host "  [FAIL] $name" -ForegroundColor Red }
}
# curl → 临时文件 → 按字节读回来 → 显式 UTF-8 解码为字符串
function RTools {
    param([string[]]$ca)
    $o = Join-Path $env:TEMP ("ftout-" + [guid]::NewGuid().ToString("N") + ".bin")
    try {
        & curl.exe -s @ca -o $o
        if (-not (Test-Path $o)) { return $null }
        return [IO.File]::ReadAllText($o, [Text.UTF8Encoding]::new($false))
    } finally { Remove-Item $o -Force -ErrorAction SilentlyContinue }
}
function CJson {
    param([string[]]$ca)
    $t = RTools $ca
    if (-not $t -or $t.Length -eq 0){ return $null }
    try { return (ConvertFrom-Json $t) } catch { Write-Host "    [json-error] $($_.Exception.Message)"; return $null }
}
function JPost($url, $body){
    $p = Join-Path $env:TEMP ("ft-" + [guid]::NewGuid().ToString("N") + ".json")
    [IO.File]::WriteAllText($p, $body, [Text.UTF8Encoding]::new($false))
    $r = CJson @('-X','POST','-H','Content-Type: application/json','--data-binary',("@$p"),$url)
    Remove-Item $p -Force -ErrorAction SilentlyContinue
    return $r
}

Write-Host "=== FileTra C 版冒烟测试 @ $u ==="

# 1) 静态页面
$html = RTools @($u)
Check "GET / 返回 HTML（含 FileTra 标题）" ($html -and ($html -match 'FileTra'))
$jsPath = Join-Path $env:TEMP ("ftappjs-" + [guid]::NewGuid().ToString("N") + ".js")
curl.exe -s -o $jsPath "$u/app.js"
$jsLen = (Get-Item $jsPath -ErrorAction SilentlyContinue).Length
Remove-Item $jsPath -Force -ErrorAction SilentlyContinue
Check "GET /app.js 长度 > 10KB（实测 $jsLen）" ($jsLen -gt 10000)

# 2) info
$info = CJson @("$u/api/info")
Check "api/info.name 非空" ($info -and $info.name -and $info.name.Length -gt 0)
Check "api/info.port == $Port" ($info.port -eq $Port)
Check "api/info.urls 数组" ($info -and ($info.urls -is [array]) -and $info.urls.Count -ge 1)
$dataDir = $info.dataDir

# 3) 列表
$list = CJson @("$u/api/list?path=")
Check "api/list 返回 entries" ($list -and ($list.entries -is [array]))
Check "api/list.path 回显 ''" ($list.path -eq "")

# 4) mkdir / 创建测试文件
$mk = JPost "$u/api/mkdir" '{"path":"测试目录C"}'
Check "mkdir OK" ($mk -and ($mk.ok -eq $true))
JPost "$u/api/mkdir" '{"path":"测试目录C"}' | Out-Null
$mk2 = JPost "$u/api/mkdir" '{"path":"测试目录C"}'
Check "mkdir 重复 -> 409/同名目录已存在" ($mk2 -and ($mk2.error -match '同名目录已存在'))

$tmp = Join-Path $PWD "tmpsmoke.txt"
[IO.File]::WriteAllText($tmp, "hello filetra c", [Text.UTF8Encoding]::new($false))

# 5) 上传 init/chunk/complete（含 offset 错配重放）
$ini = JPost "$u/api/upload/init" ('{"name":"冒烟上传.txt","size":' + (Get-Item $tmp).Length + ',"dir":"测试目录C"}')
Check "upload/init 有 id" ($ini -and $ini.id -and $ini.id.Length -eq 16)
$id = $ini.id
if ($id){
    $bytes = [IO.File]::ReadAllBytes($tmp)
    $bp = Join-Path $env:TEMP ("ft-chunk-" + [guid]::NewGuid().ToString("N") + ".bin")
    [IO.File]::WriteAllBytes($bp, $bytes)
    $ck = CJson @('-X','POST','--data-binary',("@$bp"),"$u/api/upload/chunk/${id}?offset=0")
    Check "chunk#1 received==len" ($ck -and ($ck.received -eq $bytes.Length))
    $ck3 = CJson @('-X','POST','--data-binary',("@$bp"),"$u/api/upload/chunk/${id}?offset=0")
    Check "chunk offset 回放 -> mismatch:true" ($ck3 -and ($ck3.mismatch -eq $true))
    $st = CJson @("$u/api/upload/status/$id")
    Check "status received==len" ($st -and ($st.received -eq $bytes.Length))
    $fin = CJson @('-X','POST',"$u/api/upload/complete/$id")
    Check "complete 返回 name" ($fin -and ($fin.name -eq "冒烟上传.txt") -and ($fin.size -eq $bytes.Length))
    Remove-Item $bp -Force -ErrorAction SilentlyContinue
}

# 6) 下载 + Range
$dl = RTools @("$u/d/测试目录C/冒烟上传.txt")
Check "下载内容 hello" ($dl -and ($dl -match 'hello'))
$r206 = RTools @('-H','Range: bytes=0-4',"$u/d/测试目录C/冒烟上传.txt")
Check "Range 206 -> 'hello'" ($r206 -eq 'hello')
$hdr = RTools @('-I',"$u/d/测试目录C/冒烟上传.txt")
Check "下载头含 attachment" ($hdr -and ($hdr -match 'attachment'))
$r416 = & curl.exe -s -o NUL -w "%{http_code}" -H "Range: bytes=9999-" "$u/d/测试目录C/冒烟上传.txt"
Check "越界 Range -> 416 (got $r416)" ($r416 -eq '416')

# 7) 文本
$t = JPost "$u/api/texts" '{"content":"文本C测试 ü担并.EndsWith"}'
Check "texts POST 返回 id" ($t -and ($t.id -gt 0))
$ts = CJson @("$u/api/texts")
Check "texts GET 含内容" ($ts -and ($ts.texts | Where-Object { $_.content -match '文本C测试' }))
if ($t -and $t.id){
    $td = CJson @('-X','DELETE',"$u/api/text/$($t.id)")
    Check "text DELETE ok" ($td -and ($td.ok -eq $true))
}

# 8) history（此时最新事件：texts；之前有 receive/send）
$hs = CJson @("$u/api/history")
Check "history 最新在前且为 text" ($hs -and $hs.events -and $hs.events.Count -gt 0 -and ($hs.events[0].type -eq 'text'))
if ($hs -and $hs.events){
    $types = $hs.events | ForEach-Object { $_.type }
    Check "history 含 receive/send 事件" (($types -contains 'receive') -and ($types -contains 'send'))
}
$clr = CJson @('-X','POST',"$u/api/history/clear")
Check "history clear ok" ($clr -and ($clr.ok -eq $true))

# 9) zip
$zp = Join-Path $env:TEMP ("ft-zip-" + [guid]::NewGuid().ToString("N") + ".zip")
curl.exe -s --max-time 30 -o $zp "$u/zip/测试目录C"
$zOk = Test-Path $zp
if ($zOk){
    $zb = [IO.File]::ReadAllBytes($zp)
    $ash = ($zb.Length -gt 30 -and $zb[0] -eq 0x50 -and $zb[1] -eq 0x4B)
    Check "zip PK 头（$($zb.Length) 字节）" $ash
} else { Check "zip PK 头（文件未生成）" $false }
Remove-Item $zp -Force -ErrorAction SilentlyContinue

# 10) delete 目录
$del = JPost "$u/api/delete" '{"path":"测试目录C/冒烟上传.txt"}'
Check "delete 上传文件 ok" ($del -and ($del.ok -eq $true))
$del2 = JPost "$u/api/delete" '{"path":"测试目录C"}'
Check "delete 目录 ok" ($del2 -and ($del2.ok -eq $true))

# 11) QR PNG（二进制落盘再验魔数与 320x320）
$qr = Join-Path $env:TEMP ("ft-qr-" + [guid]::NewGuid().ToString("N") + ".png")
$qrUrl = "$u/api/qr?u=" + [uri]::EscapeDataString("http://$env:COMPUTERNAME`:$Port")
curl.exe -s --max-time 20 -o $qr $qrUrl
$qbOk = Test-Path $qr
if ($qbOk){
    $qb = [IO.File]::ReadAllBytes($qr)
    $magic = ($qb.Length -gt 200 -and $qb[0] -eq 0x89 -and $qb[1] -eq 0x50 -and $qb[2] -eq 0x4E -and $qb[3] -eq 0x47)
    Check "api/qr PNG 魔数89504E47（$($qb.Length) 字节）" $magic
    if ($magic){
        $w = [int]$qb[16]*16777216 + [int]$qb[17]*65536 + [int]$qb[18]*256 + [int]$qb[19]
        Check "api/qr 宽度 320（实测 $w）" ($w -eq 320)
    }
} else { Check "api/qr PNG 魔数（文件未生成）" $false }
Remove-Item $qr -Force -ErrorAction SilentlyContinue

# 12) 穿越防护
$tr = & curl.exe -s -o NUL -w "%{http_code}" "$u/d/..%2Ftestdata%2Fsecret.txt"
Check "路径穿越被拒绝(400/404, got $tr)" ($tr -ne '200')

# 清理
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

Write-Host ("=== 完成: PASS=" + $script:PASS + " FAIL=" + $script:FAIL + " ===")
if ($script:FAIL -gt 0){ exit 1 } else { exit 0 }
