package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const tempPrefix = ".ftpart-"

// uploadSession 单个文件的上传会话
type uploadSession struct {
	mu       sync.Mutex
	id       string
	display  string // 清洗后的目标文件名
	relDir   string // 相对共享目录的子目录（"" 表示根）
	tmpPath  string
	size     int64 // 客户端声明的总大小，可为 0
	received int64
	file     *os.File
	last     time.Time
	done     bool
}

func (s *uploadSession) cleanupLocked() {
	if s.file != nil {
		_ = s.file.Close()
		s.file = nil
	}
	_ = os.Remove(s.tmpPath)
}

// UploadManager 会话表 + 超期清理
type UploadManager struct {
	mu sync.Mutex
	m  map[string]*uploadSession
}

func newUploadManager() *UploadManager {
	m := &UploadManager{m: map[string]*uploadSession{}}
	go m.janitor()
	return m
}

func (m *UploadManager) get(id string) *uploadSession {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.m[id]
}

func (m *UploadManager) put(s *uploadSession) {
	m.mu.Lock()
	m.m[s.id] = s
	m.mu.Unlock()
}

func (m *UploadManager) remove(id string) {
	m.mu.Lock()
	delete(m.m, id)
	m.mu.Unlock()
}

func (m *UploadManager) janitor() {
	t := time.NewTicker(10 * time.Minute)
	for range t.C {
		m.mu.Lock()
		var stale []*uploadSession
		for id, s := range m.m {
			s.mu.Lock()
			idle := time.Since(s.last)
			s.mu.Unlock()
			if idle > 2*time.Hour {
				stale = append(stale, s)
				delete(m.m, id)
			}
		}
		m.mu.Unlock()
		for _, s := range stale {
			s.mu.Lock()
			s.cleanupLocked()
			s.mu.Unlock()
		}
	}
}

// ---------------- handlers ----------------

func handleUploadInit(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
		Size int64  `json:"size"`
		Dir  string `json:"dir"`
	}
	if err := readJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, "请求格式错误")
		return
	}
	name := sanitizeName(body.Name)
	if name == "" {
		writeErr(w, http.StatusBadRequest, "无效的文件名")
		return
	}
	relDir := sanitizeRelDir(body.Dir)
	dirAbs, err := safePath(relDir)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的目标目录")
		return
	}
	if err := os.MkdirAll(dirAbs, 0o755); err != nil {
		writeErr(w, http.StatusInternalServerError, "无法创建目标目录")
		return
	}

	id := randHex(8)
	tmp := filepath.Join(dirAbs, tempPrefix+id+"-"+name)
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "无法创建临时文件")
		return
	}
	s := &uploadSession{
		id: id, display: name, relDir: relDir, tmpPath: tmp,
		size: body.Size, file: f, last: time.Now(),
	}
	uploads.put(s)
	writeJSON(w, http.StatusOK, map[string]any{"id": id})
}

func getUploadSession(r *http.Request) (*uploadSession, error) {
	id := r.PathValue("id")
	if len(id) != 16 || !isHex(id) {
		return nil, fmt.Errorf("无效的会话 ID")
	}
	s := uploads.get(id)
	if s == nil {
		return nil, fmt.Errorf("上传会话不存在（可能服务已重启），请重新选择文件上传")
	}
	return s, nil
}

func isHex(s string) bool {
	for _, c := range s {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return len(s) > 0
}

// handleUploadChunk 处理原始分片：offset 必须等于已接收字节数，否则拒绝写
// 入并回告进度，保证重试不会造成重复追加
func handleUploadChunk(w http.ResponseWriter, r *http.Request) {
	s, err := getUploadSession(r)
	if err != nil {
		writeErr(w, http.StatusNotFound, err.Error())
		return
	}
	offset, err := strconv.ParseInt(r.URL.Query().Get("offset"), 10, 64)
	if err != nil || offset < 0 {
		writeErr(w, http.StatusBadRequest, "无效的偏移量")
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	s.last = time.Now()
	if s.done || s.file == nil {
		writeJSON(w, http.StatusConflict, map[string]any{"error": "会话已结束", "received": s.received})
		return
	}
	if offset != s.received {
		// 重复/乱序分片：不追加，返回真实进度
		writeJSON(w, http.StatusOK, map[string]any{"received": s.received, "mismatch": true})
		return
	}
	n, err := io.Copy(s.file, r.Body)
	if n > 0 {
		s.received += n
	}
	if err != nil {
		log.Printf("写入分片出错：%v", err)
		writeJSON(w, http.StatusInternalServerError, map[string]any{"error": "写入失败", "received": s.received})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"received": s.received})
}

func handleUploadStatus(w http.ResponseWriter, r *http.Request) {
	s, err := getUploadSession(r)
	if err != nil {
		writeErr(w, http.StatusNotFound, err.Error())
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{"received": s.received, "size": s.size})
}

func handleUploadComplete(w http.ResponseWriter, r *http.Request) {
	s, err := getUploadSession(r)
	if err != nil {
		writeErr(w, http.StatusNotFound, err.Error())
		return
	}
	s.mu.Lock()
	if s.done || s.file == nil {
		received := s.received
		s.mu.Unlock()
		writeErr(w, http.StatusConflict, fmt.Sprintf("会话已结束（已接收 %d 字节）", received))
		return
	}
	_ = s.file.Close()
	s.file = nil
	s.done = true
	received := s.received
	dirAbs, derr := safePath(s.relDir)
	s.mu.Unlock()

	if derr != nil {
		writeErr(w, http.StatusInternalServerError, "目标目录不可用")
		return
	}
	final := uniqueName(dirAbs, s.display)
	if err := os.Rename(s.tmpPath, final); err != nil {
		// 跨设备等极端情况的兜底：复制后删除
		if cerr := copyFile(s.tmpPath, final); cerr != nil {
			s.mu.Lock()
			s.cleanupLocked()
			s.mu.Unlock()
			uploads.remove(s.id)
			writeErr(w, http.StatusInternalServerError, "保存文件失败")
			return
		}
		_ = os.Remove(s.tmpPath)
	}
	uploads.remove(s.id)

	st.AddEvent("receive", filepath.Base(final), received, clientIP(r))
	writeJSON(w, http.StatusOK, map[string]any{"name": filepath.Base(final), "size": received})
}

func handleUploadAbort(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if s := uploads.get(id); s != nil {
		s.mu.Lock()
		s.cleanupLocked()
		s.mu.Unlock()
		uploads.remove(id)
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// uniqueName 在目录里找不冲突的文件名，冲突时追加 (1)(2)…
func uniqueName(dir, name string) string {
	full := filepath.Join(dir, name)
	if _, err := os.Stat(full); os.IsNotExist(err) {
		return full
	}
	ext := filepath.Ext(name)
	base := strings.TrimSuffix(name, ext)
	for i := 1; ; i++ {
		cand := filepath.Join(dir, fmt.Sprintf("%s (%d)%s", base, i, ext))
		if _, err := os.Stat(cand); os.IsNotExist(err) {
			return cand
		}
	}
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}
