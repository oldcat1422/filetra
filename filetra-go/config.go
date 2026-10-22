package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// 外置配置文件 filetra.conf（与 exe 同目录）
// 优先级：配置文件 > 命令行参数 > 内置默认值
// 格式：UTF-8（可带 BOM），key=value，# 开头整行注释；空值=未设置；
//       未知键/不合法值打印提示后忽略，其余行为不变
type confT struct {
	port        int
	hasPort     bool
	name        string
	hasName     bool
	dir         string
	hasDir      bool
	password    string
	hasPassword bool
	user        string
	hasUser     bool
	noBrowser   bool
	hasNoBrowser bool
}

// confBool 与 C 版字面量集合一致：true/false、1/0、yes/no、on/off（不区分大小写）
func confBool(v string) (val, ok bool) {
	switch strings.ToLower(v) {
	case "true", "1", "yes", "on":
		return true, true
	case "false", "0", "no", "off":
		return false, true
	}
	return false, false
}

// confPath 返回 exe 同目录的 filetra.conf；拿不到 exe 路径返回 ""
func confPath() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return filepath.Join(filepath.Dir(exe), "filetra.conf")
}

// loadConf 读取并解析配置文件；文件不存在返回 (nil, nil)，一切照旧
func loadConf() (*confT, []string) {
	b, err := os.ReadFile(confPath())
	if err != nil {
		return nil, nil
	}
	if len(b) > 64*1024 {
		fmt.Fprintf(os.Stderr, "配置文件过大（上限 64KB），已忽略\n")
		return nil, nil
	}
	s := strings.TrimPrefix(string(b), "\ufeff") // 兼容 UTF-8 BOM
	var c confT
	applied := []string{}
	for i, raw := range strings.Split(s, "\n") {
		no := i + 1
		line := strings.TrimSpace(raw) // 并顺带去掉 \r（兼容 CRLF）
		if line == "" || line[0] == '#' {
			continue
		}
		eq := strings.Index(line, "=")
		if eq < 0 {
			fmt.Fprintf(os.Stderr, "配置第 %d 行缺少 '='，已忽略\n", no)
			continue
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.TrimSpace(line[eq+1:])
		if len(key) > 39 {
			fmt.Fprintf(os.Stderr, "配置第 %d 行键名过长，已忽略\n", no)
			continue
		}
		if val == "" {
			continue // 空值统一按"未设置"处理
		}
		switch strings.ToLower(key) {
		case "port":
			n, err := strconv.Atoi(val)
			if err != nil || n < 1 || n > 65535 {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 port 值无效（应为 1~65535），已忽略\n", no)
				break
			}
			c.port, c.hasPort = n, true
			applied = append(applied, "port")
		case "name":
			if len(val) >= 255 {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 name 过长，已忽略\n", no)
				break
			}
			c.name, c.hasName = val, true
			applied = append(applied, "name")
		case "dir":
			if len(val) >= 518 {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 dir 过长，已忽略\n", no)
				break
			}
			c.dir, c.hasDir = val, true
			applied = append(applied, "dir")
		case "password":
			if len(val) >= 127 {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 password 过长，已忽略\n", no)
				break
			}
			c.password, c.hasPassword = val, true
			applied = append(applied, "password")
		case "user":
			if strings.Contains(val, ":") {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 user 不能包含 ':'，已忽略\n", no)
				break
			}
			if len(val) >= 64 {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 user 过长，已忽略\n", no)
				break
			}
			c.user, c.hasUser = val, true
			applied = append(applied, "user")
		case "no-browser":
			bv, ok := confBool(val)
			if !ok {
				fmt.Fprintf(os.Stderr, "配置第 %d 行 no-browser 值无效（true/false/1/0/yes/no/on/off），已忽略\n", no)
				break
			}
			c.noBrowser, c.hasNoBrowser = bv, true
			applied = append(applied, "no-browser")
		default:
			fmt.Fprintf(os.Stderr, "配置第 %d 行未识别的键 '%s'，已忽略\n", no, key)
		}
	}
	return &c, applied
}
