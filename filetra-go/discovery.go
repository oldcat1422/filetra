package main

import (
	"encoding/json"
	"log"
	"net"
	"sort"
	"sync"
	"time"
)

const (
	beaconBase  = 52020
	beaconPorts = 8 // 每个实例绑定 beaconBase..+7 中的一个，广播时发全部端口
)

// Beacon 广播报文
type Beacon struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	URL  string `json:"url"`
}

// Device 已发现的附近设备
type Device struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	URL  string `json:"url"`
	IP   string `json:"ip"`
	Seen time.Time `json:"-"`
}

var (
	devMu   sync.Mutex
	devices = map[string]*Device{}
)

func startDiscovery() {
	go broadcastLoop()
	go listenBeacon()
}

func broadcastLoop() {
	conn, err := net.ListenUDP("udp4", nil)
	if err != nil {
		log.Printf("UDP 广播不可用：%v", err)
		return
	}
	b := Beacon{ID: instanceID, Name: deviceName}
	tick := time.NewTicker(2 * time.Second)
	defer tick.Stop()
	for range tick.C {
		b.URL = primaryURL()
		data, err := json.Marshal(b)
		if err != nil {
			continue
		}
		for _, target := range broadcastTargets() {
			if _, err := conn.WriteToUDP(data, target); err != nil {
				// 单个广播失败不影响整体
				_ = err
			}
		}
		pruneDevices()
	}
}

// broadcastTargets 本网段各接口广播地址 + 受限广播地址 + 本机回环，
// 对 beacon 端口段逐一发送，保证每个实例都能收到
func broadcastTargets() []*net.UDPAddr {
	ports := make([]int, beaconPorts)
	for i := range ports {
		ports[i] = beaconBase + i
	}
	var out []*net.UDPAddr
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		ipn, ok := a.(*net.IPNet)
		if !ok {
			continue
		}
		if bc := broadcastOf(ipn); bc != nil {
			for _, p := range ports {
				out = append(out, &net.UDPAddr{IP: bc, Port: p})
			}
		}
	}
	for _, p := range ports {
		out = append(out, &net.UDPAddr{IP: net.IPv4bcast, Port: p})
		out = append(out, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: p})
	}
	return out
}

func broadcastOf(ipn *net.IPNet) net.IP {
	ip := ipn.IP.To4()
	mask := ipn.Mask
	if ip == nil || len(mask) != 4 {
		return nil
	}
	bc := make(net.IP, 4)
	for i := 0; i < 4; i++ {
		bc[i] = ip[i] | ^mask[i]
	}
	return bc
}

func listenBeacon() {
	for i := 0; i < beaconPorts; i++ {
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{Port: beaconBase + i})
		if err == nil {
			receiveLoop(conn)
			return
		}
	}
	// 同机超多开时监听失败可接受：只广播不收集
	log.Printf("UDP 端口 %d~%d 均被占用，“附近设备”功能受限", beaconBase, beaconBase+beaconPorts-1)
}

func receiveLoop(conn *net.UDPConn) {
	buf := make([]byte, 4096)
	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			return
		}
		var b Beacon
		if json.Unmarshal(buf[:n], &b) != nil || b.ID == "" || b.ID == instanceID {
			continue
		}
		host := addr.IP.String()
		devMu.Lock()
		devices[b.ID] = &Device{ID: b.ID, Name: b.Name, URL: b.URL, IP: host, Seen: time.Now()}
		devMu.Unlock()
	}
}
func pruneDevices() {
	devMu.Lock()
	defer devMu.Unlock()
	for id, d := range devices {
		if time.Since(d.Seen) > 12*time.Second {
			delete(devices, id)
		}
	}
}

func deviceList() []Device {
	devMu.Lock()
	defer devMu.Unlock()
	out := make([]Device, 0, len(devices))
	for _, d := range devices {
		out = append(out, *d)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}
