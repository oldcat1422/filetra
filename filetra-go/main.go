package main

import (
	"crypto/subtle"
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

//go:embed web
var webFS embed.FS

var (
	rootDir    string
	dataDir    string
	deviceName string
	password   string
	port       int
	instanceID string

	st      *Store
	uploads *UploadManager
)

func main() {
	var userPort int
	var dirFlag, nameFlag, passFlag string
	var noBrowser bool
	flag.IntVar(&userPort, "port", 5050, "HTTP 端口，被占用时自动顺延")
	flag.StringVar(&dirFlag, "dir", ".", "要共享的目录")
	flag.StringVar(&nameFlag, "name", "", "设备显示名称，默认取计算机名")
	flag.StringVar(&passFlag, "password", "", "访问密码（可选），用户名固定为 filetra")
	flag.BoolVar(&noBrowser, "no-browser", false, "启动后不自动打开浏览器")
	flag.Parse()

	// 外置配置文件（exe 同目录 filetra.conf）：优先级 配置文件 > 命令行 > 内置默认值
	if c, applied := loadConf(); c != nil {
		if c.hasPort {
			userPort = c.port
		}
		if c.hasDir {
			dirFlag = c.dir
		}
		if c.hasName {
			nameFlag = c.name
		}
		if c.hasPassword {
			passFlag = c.password
		}
		if c.hasNoBrowser {
			noBrowser = c.noBrowser
		}
		fmt.Printf("配置文件 filetra.conf 生效：%s\n", strings.Join(applied, "、"))
	}

	password = passFlag
	instanceID = randHex(6)

	abs, err := filepath.Abs(dirFlag)
	if err != nil {
		log.Fatalf("无法解析共享目录：%v", err)
	}
	if info, err := os.Stat(abs); err != nil || !info.IsDir() {
		log.Fatalf("共享目录不存在或不是目录：%s", abs)
	}
	rootDir = abs

	dataDir, err = defaultDataDir()
	if err != nil {
		log.Fatalf("无法定位数据目录：%v", err)
	}
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		log.Fatalf("无法创建数据目录：%v", err)
	}

	st, err = newStore(filepath.Join(dataDir, "history.json"), filepath.Join(dataDir, "texts.json"))
	if err != nil {
		log.Fatalf("无法加载数据：%v", err)
	}

	deviceName = nameFlag
	if deviceName == "" {
		deviceName, _ = os.Hostname()
	}
	if deviceName == "" {
		deviceName = "未命名设备"
	}

	// 选择可用端口
	port = 0
	p := userPort
	for i := 0; i < 100; i++ {
		if p < 1 || p > 65535 {
			break
		}
		ln, err := net.Listen("tcp", fmt.Sprintf(":%d", p))
		if err == nil {
			ln.Close()
			port = p
			break
		}
		p++
	}
	if port == 0 {
		log.Fatalf("没有找到可用端口（从 %d 开始尝试）", userPort)
	}

	uploads = newUploadManager()

	mux := http.NewServeMux()

	// 静态资源
	sub, _ := fs.Sub(webFS, "web")
	mux.Handle("GET /app.js", staticFile(sub, "app.js", "text/javascript; charset=utf-8"))
	mux.Handle("GET /style.css", staticFile(sub, "style.css", "text/css; charset=utf-8"))
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		b, err := fs.ReadFile(sub, "index.html")
		if err != nil {
			http.Error(w, "页面资源缺失", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = w.Write(b)
	})

	// API
	mux.HandleFunc("GET /api/info", handleInfo)
	mux.HandleFunc("GET /api/list", handleList)
	mux.HandleFunc("GET /d/{path...}", handleDownload)
	mux.HandleFunc("GET /zip/{path...}", handleZip)

	mux.HandleFunc("POST /api/upload/init", handleUploadInit)
	mux.HandleFunc("POST /api/upload/chunk/{id}", handleUploadChunk)
	mux.HandleFunc("GET /api/upload/status/{id}", handleUploadStatus)
	mux.HandleFunc("POST /api/upload/complete/{id}", handleUploadComplete)
	mux.HandleFunc("POST /api/upload/abort/{id}", handleUploadAbort)

	mux.HandleFunc("GET /api/devices", handleDevices)
	mux.HandleFunc("GET /api/qr", handleQR)
	mux.HandleFunc("GET /api/texts", handleTextsGet)
	mux.HandleFunc("POST /api/texts", handleTextsPost)
	mux.HandleFunc("DELETE /api/texts", handleTextsClear)
	mux.HandleFunc("DELETE /api/text/{id}", handleTextDelete)
	mux.HandleFunc("GET /api/history", handleHistory)
	mux.HandleFunc("POST /api/history/clear", handleHistoryClear)
	mux.HandleFunc("POST /api/delete", handleDelete)
	mux.HandleFunc("POST /api/mkdir", handleMkdir)

	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", port),
		Handler:           wrapAuth(mux),
		ReadHeaderTimeout: 15 * time.Second,
	}

	startDiscovery()

	url := primaryURL()
	printBanner(url)

	if !noBrowser {
		go openBrowser(url)
	}

	if err := srv.ListenAndServe(); err != nil {
		log.Fatalf("HTTP 服务启动失败：%v", err)
	}
}

func staticFile(ffs fs.FS, name, ctype string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, err := fs.ReadFile(ffs, name)
		if err != nil {
			http.Error(w, "资源缺失", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", ctype)
		w.Header().Set("Cache-Control", "no-cache")
		_, _ = w.Write(b)
	})
}

func wrapAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if password != "" {
			user, pass, ok := r.BasicAuth()
			uOK := subtle.ConstantTimeCompare([]byte(user), []byte("filetra")) == 1
			pOK := subtle.ConstantTimeCompare([]byte(pass), []byte(password)) == 1
			if !ok || !uOK || !pOK {
				w.Header().Set("WWW-Authenticate", `Basic realm="FileTra", charset="UTF-8"`)
				http.Error(w, "需要访问密码", http.StatusUnauthorized)
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

func openBrowser(u string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", u)
	case "darwin":
		cmd = exec.Command("open", u)
	default:
		cmd = exec.Command("xdg-open", u)
	}
	_ = cmd.Start()
}

func printBanner(u string) {
	line := "─────────────────────────────────────────"
	fmt.Println()
	fmt.Println("  FileTra  ·  局域网文件传输")
	fmt.Println("  " + line)
	fmt.Printf("  共享目录 : %s\n", rootDir)
	fmt.Printf("  数据目录 : %s\n", dataDir)
	fmt.Printf("  本机地址 : %s\n", u)
	for _, ip := range lanIPv4s()[1:] {
		fmt.Printf("             http://%s:%d\n", ip, port)
	}
	if password != "" {
		fmt.Println("  访问密码 : 已启用（用户名 filetra）")
	}
	fmt.Println("  其他设备:浏览器访问上面的地址即可互传")
	fmt.Println("  " + line)
	fmt.Println()
}
