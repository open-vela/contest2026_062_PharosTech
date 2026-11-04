// SPDX-License-Identifier: Apache-2.0

package httpapi

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"nyabula.tech/cloud/internal/relay"
	"nyabula.tech/cloud/internal/stats"
	"nyabula.tech/cloud/internal/store"
	ny "nyabula.tech/nyalink"
)

// Media stays inside the existing authenticated, session-addressed envelope.
// The cloud does not decode private image/audio contents or broadcast them.
func TestCameraIntercomRelay(t *testing.T) {
	st, err := store.Open(filepath.Join(t.TempDir(), "media.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	sc := stats.New(st)
	reg := relay.New(st, sc)
	srv := httptest.NewServer(New(st, reg, sc, nil))
	defer srv.Close()
	defer reg.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	base := "ws" + strings.TrimPrefix(srv.URL, "http")
	c := &apiClient{t: t, base: srv.URL}
	status, out := c.do("POST", "/api/v1/auth/register", map[string]string{
		"email": "media@example.test", "password": "fixture-password", "name": "Media fixture",
	})
	if status != 200 {
		t.Fatalf("register: %d", status)
	}
	c.token = out["token"].(string)
	dev := dialDevice(t, ctx, base, "nya-media", "fixture-secret", "654321")
	defer dev.conn.CloseNow()
	if status, _ = c.do("POST", "/api/v1/devices/claim", map[string]string{"deviceId": "nya-media", "claimCode": "654321"}); status != 200 {
		t.Fatalf("claim: %d", status)
	}
	if unauthorized, response, err := websocket.Dial(ctx, base+"/api/v1/relay/nya-media", nil); err == nil || response == nil || response.StatusCode != 401 {
		if unauthorized != nil {
			unauthorized.CloseNow()
		}
		t.Fatalf("unauthenticated media relay accepted: %v", err)
	}
	dial := func() (*websocket.Conn, string) {
		conn, _, err := websocket.Dial(ctx, base+"/api/v1/relay/nya-media?token="+c.token, nil)
		if err != nil {
			t.Fatal(err)
		}
		conn.SetReadLimit(32768)
		_, raw, err := dev.conn.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var open muxMsg
		if json.Unmarshal(raw, &open) != nil || open.Op != "open" {
			t.Fatalf("expected channel open: %s", raw)
		}
		return conn, open.Sid
	}
	client, sid := dial()
	defer client.CloseNow()
	other, _ := dial()
	defer other.CloseNow()
	chunk := bytes.Repeat([]byte{0, 1, 2, 127, 128, 255}, 2048)
	frame, err := ny.EncodeEvent("camera.segment", map[string]any{
		"kind": "fragment", "generation": 1, "sequence": 1, "offset": 0,
		"total": len(chunk), "key": true, "data": base64.StdEncoding.EncodeToString(chunk),
	})
	if err != nil {
		t.Fatal(err)
	}
	dev.write(t, muxMsg{Op: "frame", Sid: sid, Frame: frame})
	_, raw, err := client.Read(ctx)
	if err != nil || !bytes.Equal(raw, frame) {
		t.Fatalf("video chunk changed: %v", err)
	}
	pcm := base64.StdEncoding.EncodeToString(make([]byte, 3200))
	up, _ := ny.EncodeReq("audio-1", "intercom.audio", map[string]any{"sequence": 0, "data": pcm})
	if err := client.Write(ctx, websocket.MessageText, up); err != nil {
		t.Fatal(err)
	}
	_, raw, err = dev.conn.Read(ctx)
	var received muxMsg
	if err != nil || json.Unmarshal(raw, &received) != nil || received.Sid != sid || !bytes.Equal(received.Frame, up) {
		t.Fatalf("talk packet changed or misrouted: %v", err)
	}
	down, _ := ny.EncodeEvent("intercom.audio", map[string]any{"sequence": 0, "samples": 1600, "data": pcm})
	dev.write(t, muxMsg{Op: "frame", Sid: sid, Frame: down})
	_, raw, err = client.Read(ctx)
	if err != nil || !bytes.Equal(raw, down) {
		t.Fatalf("listen packet changed: %v", err)
	}
	quiet, done := context.WithTimeout(ctx, 150*time.Millisecond)
	_, _, err = other.Read(quiet)
	done()
	if err == nil {
		t.Fatal("private media reached the other channel")
	}
	if status, _ = c.do("POST", "/api/v1/auth/logout", nil); status != 200 {
		t.Fatalf("logout: %d", status)
	}
	deadline, stop := context.WithTimeout(ctx, 2*time.Second)
	defer stop()
	if _, _, err = client.Read(deadline); err == nil || deadline.Err() != nil {
		t.Fatalf("media channel remained open after logout: %v", err)
	}
}
