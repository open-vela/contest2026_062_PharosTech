// SPDX-License-Identifier: Apache-2.0

// Package relay implements the two WebSocket sides of nyacloud:
// the outbound device link (/api/v1/device/connect, multiplexed) and the
// transparent client relay (/api/v1/relay/{deviceId}). NyaLink frames are
// forwarded opaquely; the cloud never interprets device business topics.
package relay

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
	"nyabula.tech/cloud/internal/auth"
	"nyabula.tech/cloud/internal/stats"
	"nyabula.tech/cloud/internal/store"
	ny "nyabula.tech/nyalink"
)

// muxMsg is one message on the device link (cloud.md §2).
type muxMsg struct {
	Op    string          `json:"op"`
	Sid   string          `json:"sid,omitempty"`
	Frame json.RawMessage `json:"frame,omitempty"`

	DeviceID    string `json:"deviceId,omitempty"`
	Secret      string `json:"secret,omitempty"`
	ClaimCode   string `json:"claimCode,omitempty"`
	Name        string `json:"name,omitempty"`
	CoreVersion string `json:"coreVersion,omitempty"`

	Code string `json:"code,omitempty"`
}

// session is one relayed client attached to a device link.
type session struct {
	frames chan json.RawMessage // device -> client
	done   chan struct{}
	once   sync.Once
	owner  int64
	token  string
}

func (s *session) stop() { s.once.Do(func() { close(s.done) }) }

// deviceLink is one connected device.
type deviceLink struct {
	deviceID string
	writeMu  sync.Mutex
	conn     *websocket.Conn
	ctx      context.Context
	cancel   context.CancelFunc

	mu       sync.Mutex
	sessions map[string]*session
	nextSid  int
}

func (d *deviceLink) writeJSON(v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	d.writeMu.Lock()
	defer d.writeMu.Unlock()
	ctx, cancel := context.WithTimeout(d.ctx, 5*time.Second)
	defer cancel()
	return d.conn.Write(ctx, websocket.MessageText, b)
}

// Registry tracks online devices and serves both WS endpoints.
type Registry struct {
	st    *store.Store
	stats *stats.Collector

	mu      sync.Mutex
	devices map[string]*deviceLink
}

// New creates a Registry.
func New(st *store.Store, sc *stats.Collector) *Registry {
	return &Registry{st: st, stats: sc, devices: map[string]*deviceLink{}}
}

// Online reports whether deviceID is connected and how many relay clients
// are attached.
func (r *Registry) Online(deviceID string) (online bool, clients int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	d := r.devices[deviceID]
	if d == nil {
		return false, 0
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	return true, len(d.sessions)
}

// TotalClients counts relay clients across the given devices.
func (r *Registry) TotalClients(deviceIDs []string) (online, clients int) {
	for _, id := range deviceIDs {
		if on, n := r.Online(id); on {
			online++
			clients += n
		}
	}
	return
}

// --- device side ---------------------------------------------------------

// HandleDeviceConnect serves /api/v1/device/connect.
func (r *Registry) HandleDeviceConnect(w http.ResponseWriter, req *http.Request) {
	conn, err := websocket.Accept(w, req, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	ctx, cancel := context.WithCancel(req.Context())
	defer cancel()
	defer conn.Close(websocket.StatusNormalClosure, "bye")
	conn.SetReadLimit(128 * 1024)

	link := &deviceLink{conn: conn, ctx: ctx, cancel: cancel, sessions: map[string]*session{}}

	// First frame must be op:register within 10s.
	rctx, rcancel := context.WithTimeout(ctx, 10*time.Second)
	_, raw, err := conn.Read(rctx)
	rcancel()
	if err != nil {
		return
	}
	var reg muxMsg
	if err := json.Unmarshal(raw, &reg); err != nil || reg.Op != "register" ||
		reg.DeviceID == "" || reg.Secret == "" {
		_ = link.writeJSON(muxMsg{Op: "err", Code: ny.EAuth})
		return
	}
	dev, err := r.st.DeviceByID(reg.DeviceID)
	switch {
	case err == store.ErrNotFound:
		// First contact: create the record with secret hash + claim code.
		dev = &store.Device{DeviceID: reg.DeviceID, Name: reg.Name,
			SecretHash: auth.HashSecret(reg.Secret), ClaimCode: reg.ClaimCode,
			CoreVersion: reg.CoreVersion}
		if err := r.st.CreateDevice(dev); err != nil {
			log.Printf("device create %s: %v", reg.DeviceID, err)
			_ = link.writeJSON(muxMsg{Op: "err", Code: ny.EAuth})
			return
		}
	case err != nil:
		log.Printf("device lookup %s: %v", reg.DeviceID, err)
		return
	case dev.SecretHash != auth.HashSecret(reg.Secret):
		_ = link.writeJSON(muxMsg{Op: "err", Code: ny.EAuth})
		return
	}
	link.deviceID = reg.DeviceID

	r.mu.Lock()
	if r.devices[reg.DeviceID] != nil {
		r.mu.Unlock()
		_ = link.writeJSON(muxMsg{Op: "err", Code: ny.EConflict})
		return
	}
	r.devices[reg.DeviceID] = link
	r.mu.Unlock()
	if err := link.writeJSON(muxMsg{Op: "registered"}); err != nil {
		r.dropDevice(link)
		return
	}
	_ = r.st.TouchDevice(reg.DeviceID, reg.Name, reg.CoreVersion)
	r.stats.DeviceOnline(reg.DeviceID)
	log.Printf("device %s online", reg.DeviceID)
	defer func() {
		r.dropDevice(link)
		r.stats.DeviceOffline(reg.DeviceID)
		_ = r.st.TouchDevice(reg.DeviceID, reg.Name, reg.CoreVersion)
		log.Printf("device %s offline", reg.DeviceID)
	}()

	// 30s keepalive ping.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				_ = link.writeJSON(muxMsg{Op: "ping"})
			}
		}
	}()

	for {
		readCtx, readCancel := context.WithTimeout(ctx, 75*time.Second)
		_, raw, err := conn.Read(readCtx)
		readCancel()
		if err != nil {
			return
		}
		var m muxMsg
		if err := json.Unmarshal(raw, &m); err != nil {
			continue
		}
		switch m.Op {
		case "close":
			link.mu.Lock()
			s := link.sessions[m.Sid]
			link.mu.Unlock()
			if s != nil {
				s.stop()
			}
		case "frame":
			link.mu.Lock()
			s := link.sessions[m.Sid]
			link.mu.Unlock()
			if s != nil {
				r.stats.FrameUp(link.deviceID)
				select {
				case s.frames <- m.Frame:
				case <-s.done:
				default:
					s.stop() // A slow client must not block the whole device.
				}
			}
		case "cast":
			link.mu.Lock()
			targets := make([]*session, 0, len(link.sessions))
			for _, s := range link.sessions {
				targets = append(targets, s)
			}
			link.mu.Unlock()
			for _, s := range targets {
				r.stats.FrameUp(link.deviceID)
				select {
				case s.frames <- m.Frame:
				case <-s.done:
				default:
					s.stop()
				}
			}
		case "ping":
			_ = link.writeJSON(muxMsg{Op: "pong"})
		case "pong":
			// keepalive ack
		}
	}
}

func (r *Registry) dropDevice(link *deviceLink) {
	r.mu.Lock()
	if r.devices[link.deviceID] == link {
		delete(r.devices, link.deviceID)
	}
	r.mu.Unlock()
	link.mu.Lock()
	sess := link.sessions
	link.sessions = map[string]*session{}
	link.mu.Unlock()
	for _, s := range sess {
		s.stop()
	}
	link.cancel()
}

// --- client side ---------------------------------------------------------

// HandleRelay serves /api/v1/relay/{deviceId}. Auth and ownership are
// checked by the HTTP layer before this is called.
func (r *Registry) HandleRelay(w http.ResponseWriter, req *http.Request, deviceID string, owner int64, token string) {
	conn, err := websocket.Accept(w, req, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	ctx, cancel := context.WithCancel(req.Context())
	defer cancel()
	defer conn.Close(websocket.StatusNormalClosure, "bye")
	conn.SetReadLimit(64 * 1024)

	r.mu.Lock()
	link := r.devices[deviceID]
	if link == nil {
		r.mu.Unlock()
		// Per cloud.md §3: upgrade succeeds, then an err frame EOFFLINE.
		if b, err := ny.EncodeErr("", "sys", ny.EOffline, "device offline", nil); err == nil {
			_ = conn.Write(ctx, websocket.MessageText, b)
		}
		return
	}

	// Attach a session.
	// Recheck while attachment is serialized with revocation. The HTTP
	// ownership check may have raced with logout or unclaim.
	dev, devErr := r.st.DeviceByID(deviceID)
	user, userErr := r.st.SessionUser(token)
	if devErr != nil || userErr != nil || dev.OwnerID != owner || user.ID != owner {
		r.mu.Unlock()
		return
	}
	s := &session{frames: make(chan json.RawMessage, 64), done: make(chan struct{}), owner: owner, token: token}
	link.mu.Lock()
	link.nextSid++
	sid := fmt.Sprintf("s%d", link.nextSid)
	link.sessions[sid] = s
	link.mu.Unlock()
	r.mu.Unlock()
	defer func() {
		s.stop()
		link.mu.Lock()
		if link.sessions[sid] == s {
			delete(link.sessions, sid)
		}
		link.mu.Unlock()
		_ = link.writeJSON(muxMsg{Op: "close", Sid: sid})
	}()
	if err := link.writeJSON(muxMsg{Op: "open", Sid: sid}); err != nil {
		return
	}

	// device -> client pump.
	go func() {
		defer cancel() // Wake the reader when the device or writer goes away.
		for {
			select {
			case <-s.done:
				return
			case <-ctx.Done():
				return
			case f := <-s.frames:
				writeCtx, writeCancel := context.WithTimeout(ctx, 5*time.Second)
				err := conn.Write(writeCtx, websocket.MessageText, []byte(f))
				writeCancel()
				if err != nil {
					return
				}
			}
		}
	}()

	// client -> device pump (this goroutine).
	for {
		var raw []byte
		typ, raw, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if typ != websocket.MessageText {
			continue
		}
		if !json.Valid(raw) {
			return
		}
		select {
		case <-s.done:
			// Device went away mid-session.
			if b, e := ny.EncodeErr("", "sys", ny.EOffline, "device offline", nil); e == nil {
				_ = conn.Write(ctx, websocket.MessageText, b)
			}
			return
		default:
		}
		r.stats.FrameDown(deviceID)
		if link.writeJSON(muxMsg{Op: "frame", Sid: sid, Frame: json.RawMessage(raw)}) != nil {
			return
		}
	}
}

// Revoke closes existing sessions matching a released device or login.
// Empty deviceID/token are wildcards; owner always identifies the account.
func (r *Registry) Revoke(owner int64, deviceID, token string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for id, link := range r.devices {
		if deviceID != "" && id != deviceID {
			continue
		}
		link.mu.Lock()
		for _, s := range link.sessions {
			if s.owner == owner && (token == "" || token == s.token) {
				s.stop()
			}
		}
		link.mu.Unlock()
	}
}

// Close disconnects upgraded sockets, which http.Server.Shutdown does not own.
func (r *Registry) Close() {
	r.mu.Lock()
	links := make([]*deviceLink, 0, len(r.devices))
	for _, link := range r.devices {
		links = append(links, link)
	}
	r.mu.Unlock()
	for _, link := range links {
		r.dropDevice(link)
		link.conn.CloseNow()
	}
}
