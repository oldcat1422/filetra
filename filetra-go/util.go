package main

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// randHex 生成 n 字节的随机十六进制串
func randHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		// 极端情况下退化为时间种子
		return fmt.Sprintf("%x", time.Now().UnixNano())
	}
	return hex.EncodeToString(b)
}

func defaultDataDir() (string, error) {
	if base, err := os.UserConfigDir(); err == nil {
		return filepath.Join(base, "filetra"), nil
	}
	return filepath.Join(os.TempDir(), "filetra"), nil
}

// safePath 将相对路径限制在共享目录内，防止路径穿越
func safePath(rel string) (string, error) {
	rel = strings.Trim(strings.TrimSpace(rel), "/\\")
	root := filepath.Clean(rootDir)
	abs := root
	if rel != "" {
		abs = filepath.Clean(filepath.Join(root, filepath.FromSlash(rel)))
	}
	if abs == root || strings.HasPrefix(abs, root+string(os.PathSeparator)) {
		return abs, nil
	}
	return "", fmt.Errorf("非法路径")
}

// sanitizeName 清洗文件/目录名：去掉路径分隔符与 Windows 不允许的字符
func sanitizeName(name string) string {
	name = strings.ReplaceAll(name, "\\", "/")
	name = filepath.Base(name)
	if name == "." || name == ".." {
		return ""
	}
	var sb strings.Builder
	for _, r := range name {
		if r < 32 || strings.ContainsRune(`<>:"/\|?*`, r) {
			sb.WriteRune('_')
			continue
		}
		sb.WriteRune(r)
	}
	name = sb.String()
	if l := len(name); l > 200 {
		name = name[:200]
	}
	name = strings.TrimRight(name, " .")
	return name
}

// sanitizeRelDir 清洗相对目录（形如 "docs/子目录"），用于文件夹上传
func sanitizeRelDir(dir string) string {
	segs := strings.FieldsFunc(strings.ReplaceAll(dir, "\\", "/"), func(r rune) bool { return r == '/' })
	var out []string
	for _, s := range segs {
		s = sanitizeName(s)
		if s == "" {
			continue
		}
		out = append(out, s)
		if len(out) >= 20 {
			break
		}
	}
	return strings.Join(out, "/")
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// lanIPv4s 返回本机非回环 IPv4，私网地址优先
func lanIPv4s() []string {
	var ips []string
	var private []string
	var public []string
	ifaces, _ := net.Interfaces()
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := ifc.Addrs()
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipn.IP.To4()
			if ip4 == nil {
				continue
			}
			if isPrivateIP(ip4) {
				private = append(private, ip4.String())
			} else {
				public = append(public, ip4.String())
			}
		}
	}
	sort.Strings(private)
	sort.Strings(public)
	ips = append(private, public...)
	if len(ips) == 0 {
		ips = []string{"127.0.0.1"}
	}
	return ips
}

func isPrivateIP(ip net.IP) bool {
	// 10/8 、172.16/12 、192.168/16
	return ip[0] == 10 ||
		(ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
		(ip[0] == 192 && ip[1] == 168)
}

func primaryIP() string { return lanIPv4s()[0] }

// primaryURL 默认入口地址（二维码、自动打开浏览器用）
func primaryURL() string {
	return fmt.Sprintf("http://%s:%d", primaryIP(), port)
}
