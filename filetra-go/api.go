package main

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/skip2/go-qrcode"
)

// ---------------- 基础工具 ----------------

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]any{"error": msg})
}

func readJSON(r *http.Request, v any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	if err := dec.Decode(v); err != nil {
		return err
	}
	return nil
}

// ---------------- 文件列表 ----------------

type entry struct {
	Name    string `json:"name"`
	Size    int64  `json:"size"`
	ModTime int64  `json:"mtime"`
	IsDir   bool   `json:"isDir"`
}

func handleInfo(w http.ResponseWriter, r *http.Request) {
	urls := make([]string, 0, len(lanIPv4s()))
	for _, ip := range lanIPv4s() {
		urls = append(urls, fmt.Sprintf("http://%s:%d", ip, port))
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"name":        deviceName,
		"root":        rootDir,
		"dataDir":     dataDir,
		"port":        port,
		"urls":        urls,
		"hasPassword": password != "",
	})
}

func handleList(w http.ResponseWriter, r *http.Request) {
	absp, err := safePath(r.URL.Query().Get("path"))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "非法路径")
		return
	}
	dir, err := os.Open(absp)
	if err != nil {
		writeErr(w, http.StatusNotFound, "无法读取目录")
		return
	}
	defer dir.Close()
	names, err := dir.ReadDir(-1)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "读取目录失败")
		return
	}
	list := make([]entry, 0, len(names))
	for _, d := range names {
		name := d.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		info, err := d.Info()
		if err != nil {
			continue
		}
		list = append(list, entry{
			Name:    name,
			Size:    info.Size(),
			ModTime: info.ModTime().Unix(),
			IsDir:   d.IsDir(),
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"path":    r.URL.Query().Get("path"),
		"entries": list,
	})
}

// ---------------- 下载 ----------------

// handleDownload 单文件下载（http.ServeContent 自带 Range 断点续传）
func handleDownload(w http.ResponseWriter, r *http.Request) {
	rel := r.PathValue("path")
	absp, err := safePath(rel)
	if err != nil {
		http.Error(w, "非法路径", http.StatusBadRequest)
		return
	}
	info, err := os.Stat(absp)
	if err != nil || info.IsDir() {
		http.Error(w, "文件不存在（目录请使用打包下载）", http.StatusNotFound)
		return
	}
	f, err := os.Open(absp)
	if err != nil {
		http.Error(w, "无法打开文件", http.StatusForbidden)
		return
	}
	defer f.Close()

	name := filepath.Base(absp)
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
	http.ServeContent(w, r, name, info.ModTime(), f)

	if r.Method == http.MethodGet {
		st.AddEvent("send", name, info.Size(), clientIP(r))
	}
}

// handleZip 流式打包目录为 zip 下载（不落盘、不吃内存）
func handleZip(w http.ResponseWriter, r *http.Request) {
	rel := r.PathValue("path")
	absp, err := safePath(rel)
	if err != nil {
		http.Error(w, "非法路径", http.StatusBadRequest)
		return
	}
	info, err := os.Stat(absp)
	if err != nil || !info.IsDir() {
		http.Error(w, "目录不存在", http.StatusNotFound)
		return
	}
	zname := filepath.Base(absp) + ".zip"
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": zname}))

	cw := newCountingWriter(w)
	zw := zip.NewWriter(cw)
	root := absp
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil // 跳过无法访问的条目
		}
		base := d.Name()
		if d.IsDir() {
			return nil
		}
		if strings.HasPrefix(base, ".") || strings.HasPrefix(base, tempPrefix) {
			return nil
		}
		relPath, err := filepath.Rel(root, p)
		if err != nil {
			return nil
		}
		src, err := os.Open(p)
		if err != nil {
			return nil
		}
		defer src.Close()
		h := &zip.FileHeader{Name: filepath.ToSlash(relPath), Method: zip.Deflate}
		if fi, err := d.Info(); err == nil {
			h.Modified = fi.ModTime()
		}
		dst, err := zw.CreateHeader(h)
		if err != nil {
			return err
		}
		if _, err := io.Copy(dst, src); err != nil {
			return err
		}
		return nil
	})
	_ = zw.Close()
	if r.Method == http.MethodGet {
		st.AddEvent("send", zname, cw.n, clientIP(r))
	}
}

type countingWriter struct {
	w io.Writer
	n int64
}

func newCountingWriter(w io.Writer) *countingWriter { return &countingWriter{w: w} }

func (c *countingWriter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	c.n += int64(n)
	return n, err
}

// 二维码：/api/qr?u=http://...
func handleQR(w http.ResponseWriter, r *http.Request) {
	u := r.URL.Query().Get("u")
	if u == "" || (!strings.HasPrefix(u, "http://") && !strings.HasPrefix(u, "https://")) || len(u) > 500 {
		u = primaryURL()
	}
	qr, err := qrcode.New(u, qrcode.Medium)
	if err != nil {
		http.Error(w, "生成二维码失败", http.StatusInternalServerError)
		return
	}
	png, err := qr.PNG(320)
	if err != nil {
		http.Error(w, "生成二维码失败", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "no-cache")
	_, _ = w.Write(png)
}

// ---------------- 设备发现 ----------------

func handleDevices(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"devices": deviceList()})
}

// ---------------- 文本分享 ----------------

func handleTextsGet(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"texts": st.Texts()})
}

func handleTextsPost(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Content string `json:"content"`
	}
	if err := readJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, "请求格式错误")
		return
	}
	content := strings.TrimSpace(body.Content)
	if content == "" {
		writeErr(w, http.StatusBadRequest, "内容不能为空")
		return
	}
	if len(content) > maxTextSize {
		writeErr(w, http.StatusBadRequest, "内容过长（上限 200KB）")
		return
	}
	item := st.AddText(content)
	st.AddEvent("text", previewOf(content), int64(len(content)), clientIP(r))
	writeJSON(w, http.StatusOK, item)
}

func previewOf(s string) string {
	r := []rune(s)
	if len(r) > 60 {
		r = append(r[:60], []rune("…")...)
	}
	return string(r)
}

func handleTextDelete(w http.ResponseWriter, r *http.Request) {
	var idv int64
	if _, err := fmt.Sscanf(r.PathValue("id"), "%d", &idv); err != nil {
		writeErr(w, http.StatusBadRequest, "无效 ID")
		return
	}
	st.DeleteText(idv)
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func handleTextsClear(w http.ResponseWriter, r *http.Request) {
	st.ClearTexts()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// ---------------- 历史 ----------------

func handleHistory(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"events": st.Events()})
}

func handleHistoryClear(w http.ResponseWriter, r *http.Request) {
	st.ClearEvents()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// ---------------- 删除 / 新建目录 ----------------

func handleDelete(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Path string `json:"path"`
	}
	if err := readJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, "请求格式错误")
		return
	}
	absp, err := safePath(body.Path)
	if err != nil || filepath.Clean(absp) == filepath.Clean(rootDir) {
		writeErr(w, http.StatusBadRequest, "非法路径")
		return
	}
	info, err := os.Stat(absp)
	if err != nil {
		writeErr(w, http.StatusNotFound, "目标不存在")
		return
	}
	if err := os.RemoveAll(absp); err != nil {
		writeErr(w, http.StatusInternalServerError, "删除失败")
		return
	}
	typ := "delete"
	name := filepath.Base(absp)
	if info.IsDir() {
		name += "/"
	}
	st.AddEvent(typ, name, info.Size(), clientIP(r))
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func handleMkdir(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Path string `json:"path"` // 新目录名
		Dir  string `json:"dir"`  // 当前浏览的子目录，空=根目录
	}
	if err := readJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, "请求格式错误")
		return
	}
	name := sanitizeName(body.Path)
	if name == "" {
		writeErr(w, http.StatusBadRequest, "无效的目录名")
		return
	}
	rel := name
	if d := sanitizeRelDir(body.Dir); d != "" {
		rel = d + "/" + name
	}
	absp, err := safePath(rel)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "非法路径")
		return
	}
	if err := os.MkdirAll(filepath.Dir(absp), 0o755); err != nil {
		writeErr(w, http.StatusInternalServerError, "创建失败")
		return
	}
	if err := os.Mkdir(absp, 0o755); err != nil {
		if os.IsExist(err) {
			writeErr(w, http.StatusConflict, "同名目录已存在")
			return
		}
		writeErr(w, http.StatusInternalServerError, "创建失败")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}
