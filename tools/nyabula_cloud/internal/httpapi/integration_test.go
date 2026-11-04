// SPDX-License-Identifier: Apache-2.0

package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
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

// muxMsg mirrors the device-link message for the fake device.
type muxMsg struct {
	Op          string          `json:"op"`
	Sid         string          `json:"sid,omitempty"`
	Frame       json.RawMessage `json:"frame,omitempty"`
	DeviceID    string          `json:"deviceId,omitempty"`
	Secret      string          `json:"secret,omitempty"`
	ClaimCode   string          `json:"claimCode,omitempty"`
	Name        string          `json:"name,omitempty"`
	CoreVersion string          `json:"coreVersion,omitempty"`
	Code        string          `json:"code,omitempty"`
}

// fakeDevice is a handwritten NyaLink device on the multiplexed link.
type fakeDevice struct {
	conn *websocket.Conn
	ctx  context.Context
}

func dialDevice(t *testing.T, ctx context.Context, wsBase, id, secret, claim string) *fakeDevice {
	t.Helper()
	conn, _, err := websocket.Dial(ctx, wsBase+"/api/v1/device/connect", nil)
	if err != nil {
		t.Fatalf("device dial: %v", err)
	}
	b, _ := json.Marshal(muxMsg{Op: "register", DeviceID: id, Secret: secret,
		ClaimCode: claim, Name: "FakeDev", CoreVersion: "0.0.1"})
	if err := conn.Write(ctx, websocket.MessageText, b); err != nil {
		t.Fatal(err)
	}
	_, raw, err := conn.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var m muxMsg
	_ = json.Unmarshal(raw, &m)
	if m.Op != "registered" {
		t.Fatalf("register failed: %s", raw)
	}
	return &fakeDevice{conn: conn, ctx: ctx}
}

func (d *fakeDevice) write(t *testing.T, v any) {
	t.Helper()
	b, _ := json.Marshal(v)
	if err := d.conn.Write(d.ctx, websocket.MessageText, b); err != nil {
		t.Fatalf("device write: %v", err)
	}
}

// serve answers hello/pair/ping on any sid until the context ends.
func (d *fakeDevice) serve(t *testing.T) {
	for {
		_, raw, err := d.conn.Read(d.ctx)
		if err != nil {
			return
		}
		var m muxMsg
		if json.Unmarshal(raw, &m) != nil {
			continue
		}
		switch m.Op {
		case "ping":
			d.write(t, muxMsg{Op: "pong"})
		case "frame":
			env, err := ny.DecodeEnvelope(m.Frame)
			if err != nil {
				continue
			}
			var res []byte
			switch env.Topic {
			case "sys.hello":
				res, _ = ny.EncodeRes(env.ID, env.Topic, map[string]any{"pairingRequired": true})
			case "sys.pair":
				res, _ = ny.EncodeRes(env.ID, env.Topic, map[string]any{"token": "devtok", "role": "owner"})
			case "sys.ping":
				res, _ = ny.EncodeRes(env.ID, env.Topic, map[string]any{"t": time.Now().UnixMilli()})
			default:
				res, _ = ny.EncodeErr(env.ID, env.Topic, ny.ENotFound, "unknown topic", nil)
			}
			d.write(t, muxMsg{Op: "frame", Sid: m.Sid, Frame: res})
		}
	}
}

func (d *fakeDevice) castEyeState(t *testing.T) {
	f, _ := ny.EncodeEvent("eye.state", ny.EyeState{Seq: 1, T: time.Now().UnixMilli(),
		Expression: ny.Expression{Mode: "happy"}, Gaze: ny.Gaze{Mode: "auto"}})
	d.write(t, muxMsg{Op: "cast", Frame: f})
}

// --- HTTP helpers --------------------------------------------------------

type apiClient struct {
	t     *testing.T
	base  string
	token string
}

func (c *apiClient) do(method, path string, body any) (int, map[string]any) {
	c.t.Helper()
	var rd *bytes.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	} else {
		rd = bytes.NewReader(nil)
	}
	req, _ := http.NewRequest(method, c.base+path, rd)
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		c.t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

// --- the test ------------------------------------------------------------

func TestCloudEndToEnd(t *testing.T) {
	st, err := store.Open(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	collector := stats.New(st)
	registry := relay.New(st, collector)
	srv := httptest.NewServer(New(st, registry, collector, nil))
	defer srv.Close()
	wsBase := "ws" + strings.TrimPrefix(srv.URL, "http")

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// 1. Register + login + me.
	c := &apiClient{t: t, base: srv.URL}
	code, out := c.do("POST", "/api/v1/auth/register",
		map[string]string{"email": "a@b.c", "password": "hunter2", "name": "A"})
	if code != 200 || out["token"] == "" {
		t.Fatalf("register: %d %v", code, out)
	}
	code, out = c.do("POST", "/api/v1/auth/login",
		map[string]string{"email": "a@b.c", "password": "hunter2"})
	if code != 200 {
		t.Fatalf("login: %d %v", code, out)
	}
	c.token = out["token"].(string)
	code, out = c.do("GET", "/api/v1/auth/me", nil)
	if code != 200 || out["user"].(map[string]any)["email"] != "a@b.c" {
		t.Fatalf("me: %d %v", code, out)
	}
	// bad password
	if code, _ := c.do("POST", "/api/v1/auth/login",
		map[string]string{"email": "a@b.c", "password": "wrong"}); code != 401 {
		t.Fatalf("bad login should 401, got %d", code)
	}

	// 2. Fake device connects and registers.
	dev := dialDevice(t, ctx, wsBase, "nya-test0001", "s3cret", "654321")
	go dev.serve(t)

	// wrong secret is rejected
	conn2, _, err := websocket.Dial(ctx, wsBase+"/api/v1/device/connect", nil)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := json.Marshal(muxMsg{Op: "register", DeviceID: "nya-test0001", Secret: "wrong"})
	_ = conn2.Write(ctx, websocket.MessageText, b)
	_, raw, err := conn2.Read(ctx)
	if err == nil {
		var m muxMsg
		_ = json.Unmarshal(raw, &m)
		if m.Op != "err" || m.Code != ny.EAuth {
			t.Fatalf("wrong secret should EAUTH: %s", raw)
		}
	}
	conn2.Close(websocket.StatusNormalClosure, "")

	// 3. Claim: wrong code -> 403, right code -> 200, list shows online.
	if code, out := c.do("POST", "/api/v1/devices/claim",
		map[string]string{"deviceId": "nya-test0001", "claimCode": "000000"}); code != 403 {
		t.Fatalf("wrong claim code should 403: %d %v", code, out)
	}
	code, out = c.do("POST", "/api/v1/devices/claim",
		map[string]string{"deviceId": "nya-test0001", "claimCode": "654321"})
	if code != 200 {
		t.Fatalf("claim: %d %v", code, out)
	}
	code, out = c.do("GET", "/api/v1/devices", nil)
	devs := out["devices"].([]any)
	if code != 200 || len(devs) != 1 || devs[0].(map[string]any)["online"] != true {
		t.Fatalf("devices: %d %v", code, out)
	}

	// 4. Relay bridge: hello -> pair -> ping, then a cast eye.state event.
	relayURL := fmt.Sprintf("%s/api/v1/relay/%s?token=%s", wsBase, "nya-test0001", c.token)
	rc, _, err := websocket.Dial(ctx, relayURL, nil)
	if err != nil {
		t.Fatalf("relay dial: %v", err)
	}
	defer rc.Close(websocket.StatusNormalClosure, "")
	sendReq := func(id, topic string, data any) {
		f, _ := ny.EncodeReq(id, topic, data)
		if err := rc.Write(ctx, websocket.MessageText, f); err != nil {
			t.Fatal(err)
		}
	}
	awaitID := func(id string) *ny.Envelope {
		for {
			_, raw, err := rc.Read(ctx)
			if err != nil {
				t.Fatalf("relay read: %v", err)
			}
			env, err := ny.DecodeEnvelope(raw)
			if err != nil {
				t.Fatal(err)
			}
			if env.ID == id {
				return env
			}
		}
	}
	sendReq("r1", "sys.hello", map[string]any{"client": "test", "version": "1", "token": nil})
	if env := awaitID("r1"); env.Type != "res" {
		t.Fatalf("hello over relay: %+v", env)
	}
	sendReq("r2", "sys.pair", map[string]any{"code": "000000", "name": "t"})
	env := awaitID("r2")
	var pair struct{ Token, Role string }
	_ = json.Unmarshal(env.Data, &pair)
	if env.Type != "res" || pair.Token != "devtok" || pair.Role != "owner" {
		t.Fatalf("pair over relay: %+v %+v", env, pair)
	}
	sendReq("r3", "sys.ping", map[string]any{})
	if env := awaitID("r3"); env.Type != "res" {
		t.Fatalf("ping over relay: %+v", env)
	}
	dev.castEyeState(t)
	for {
		_, raw, err := rc.Read(ctx)
		if err != nil {
			t.Fatalf("waiting for cast: %v", err)
		}
		e, _ := ny.DecodeEnvelope(raw)
		if e != nil && e.Type == "event" && e.Topic == "eye.state" {
			break
		}
	}

	// 5. Stats have data.
	code, out = c.do("GET", "/api/v1/devices/nya-test0001/stats", nil)
	if code != 200 {
		t.Fatalf("stats: %d %v", code, out)
	}
	daily := out["daily"].([]any)
	if len(daily) == 0 {
		t.Fatalf("no daily stats: %v", out)
	}
	d0 := daily[0].(map[string]any)
	if d0["framesUp"].(float64) < 3 || d0["framesDown"].(float64) < 3 {
		t.Fatalf("frame counters too low: %v", d0)
	}
	cur := out["current"].(map[string]any)
	if cur["online"] != true || cur["clients"].(float64) != 1 {
		t.Fatalf("current: %v", cur)
	}
	code, out = c.do("GET", "/api/v1/stats/overview", nil)
	if code != 200 || out["devices"].(float64) != 1 || out["online"].(float64) != 1 ||
		out["framesToday"].(float64) < 6 {
		t.Fatalf("overview: %d %v", code, out)
	}

	// 6. Foreign user cannot relay.
	c2 := &apiClient{t: t, base: srv.URL}
	_, out2 := c2.do("POST", "/api/v1/auth/register",
		map[string]string{"email": "x@y.z", "password": "hunter2", "name": "X"})
	badURL := fmt.Sprintf("%s/api/v1/relay/%s?token=%s", wsBase, "nya-test0001", out2["token"])
	if _, resp, err := websocket.Dial(ctx, badURL, nil); err == nil {
		t.Fatal("foreign relay should be refused")
	} else if resp != nil && resp.StatusCode != 403 {
		t.Fatalf("foreign relay: want 403, got %d", resp.StatusCode)
	}

	// 7. Device goes offline; a fresh relay gets an EOFFLINE err frame.
	dev.conn.Close(websocket.StatusNormalClosure, "")
	deadline := time.Now().Add(5 * time.Second)
	for {
		if on, _ := registry.Online("nya-test0001"); !on {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("device never went offline")
		}
		time.Sleep(20 * time.Millisecond)
	}
	rc2, _, err := websocket.Dial(ctx, relayURL, nil)
	if err != nil {
		t.Fatalf("relay dial (offline): %v", err)
	}
	defer rc2.Close(websocket.StatusNormalClosure, "")
	_, raw, err = rc2.Read(ctx)
	if err != nil {
		t.Fatalf("offline err frame read: %v", err)
	}
	oenv, _ := ny.DecodeEnvelope(raw)
	var ed ny.ErrData
	_ = json.Unmarshal(oenv.Data, &ed)
	if oenv.Type != "err" || ed.Code != ny.EOffline {
		t.Fatalf("expected EOFFLINE, got %s", raw)
	}

	// 8. Unclaim.
	if code, out := c.do("DELETE", "/api/v1/devices/nya-test0001", nil); code != 200 {
		t.Fatalf("unclaim: %d %v", code, out)
	}
	if code, out := c.do("GET", "/api/v1/devices", nil); code != 200 ||
		len(out["devices"].([]any)) != 0 {
		t.Fatalf("devices after unclaim: %d %v", code, out)
	}
}
