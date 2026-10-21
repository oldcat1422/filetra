package main

import (
	"encoding/json"
	"log"
	"os"
	"sync"
	"time"
)

// Event 传输记录
type Event struct {
	Time int64  `json:"time"`
	Type string `json:"type"` // send | receive | text | delete
	Name string `json:"name"`
	Size int64  `json:"size"`
	Peer string `json:"peer"`
}

// TextItem 文本分享条目
type TextItem struct {
	ID      int64  `json:"id"`
	Content string `json:"content"`
	Time    int64  `json:"time"`
}

const (
	maxHistory  = 500
	maxTexts    = 100
	maxTextSize = 200 * 1024
)

// Store :历史记录与文本分享的内存态 + JSON 持久化
type Store struct {
	mu          sync.Mutex
	historyPath string
	textsPath   string
	history     []Event // 追加，尾部最新
	texts       []TextItem // 追加，尾部最新
	nextTextID  int64
}

func newStore(historyPath, textsPath string) (*Store, error) {
	s := &Store{historyPath: historyPath, textsPath: textsPath, nextTextID: 1}
	_ = loadJSON(historyPath, &s.history)
	_ = loadJSON(textsPath, &s.texts)
	for _, t := range s.texts {
		if t.ID >= s.nextTextID {
			s.nextTextID = t.ID + 1
		}
	}
	return s, nil
}

func loadJSON(path string, v any) error {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	defer f.Close()
	return json.NewDecoder(f).Decode(v)
}

func saveJSON(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// ---------------- 历史 ----------------

func (s *Store) AddEvent(typ, name string, size int64, peer string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.history = append(s.history, Event{
		Time: time.Now().Unix(), Type: typ, Name: name, Size: size, Peer: peer,
	})
	if len(s.history) > maxHistory {
		s.history = s.history[len(s.history)-maxHistory:]
	}
	if err := saveJSON(s.historyPath, s.history); err != nil {
		log.Printf("保存历史失败：%v", err)
	}
}

func (s *Store) Events() []Event {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]Event, len(s.history))
	// 倒序拷贝：最新在前
	for i, e := range s.history {
		out[len(s.history)-1-i] = e
	}
	return out
}

func (s *Store) ClearEvents() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.history = nil
	_ = saveJSON(s.historyPath, s.history)
}

// ---------------- 文本 ----------------

func (s *Store) AddText(content string) TextItem {
	s.mu.Lock()
	defer s.mu.Unlock()
	item := TextItem{ID: s.nextTextID, Content: content, Time: time.Now().Unix()}
	s.nextTextID++
	s.texts = append(s.texts, item)
	if len(s.texts) > maxTexts {
		s.texts = s.texts[len(s.texts)-maxTexts:]
	}
	if err := saveJSON(s.textsPath, s.texts); err != nil {
		log.Printf("保存文本失败：%v", err)
	}
	return item
}

func (s *Store) Texts() []TextItem {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]TextItem, len(s.texts))
	for i, t := range s.texts {
		out[len(s.texts)-1-i] = t
	}
	return out
}

func (s *Store) DeleteText(id int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, t := range s.texts {
		if t.ID == id {
			s.texts = append(s.texts[:i], s.texts[i+1:]...)
			break
		}
	}
	_ = saveJSON(s.textsPath, s.texts)
}

func (s *Store) ClearTexts() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.texts = nil
	_ = saveJSON(s.textsPath, s.texts)
}
