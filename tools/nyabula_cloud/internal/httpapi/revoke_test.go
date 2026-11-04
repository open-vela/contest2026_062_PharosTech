// SPDX-License-Identifier: Apache-2.0

package httpapi

import (
	"context"
	"encoding/json"
	"github.com/coder/websocket"
	"net/http"
	"net/http/httptest"
	"nyabula.tech/cloud/internal/relay"
	"nyabula.tech/cloud/internal/stats"
	"nyabula.tech/cloud/internal/store"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRelayRevocation(t *testing.T) {
	for _, action := range []string{"logout", "unclaim", "offline", "device-close"} {
		t.Run(action, func(t *testing.T) {
			st, err := store.Open(filepath.Join(t.TempDir(), "relay.db"))
			if err != nil {
				t.Fatal(err)
			}
			defer st.Close()
			sc := stats.New(st)
			reg := relay.New(st, sc)
			srv := httptest.NewServer(New(st, reg, sc, nil))
			defer srv.Close()
			defer reg.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			base := "ws" + strings.TrimPrefix(srv.URL, "http")
			c := &apiClient{t: t, base: srv.URL}
			status, out := c.do("POST", "/api/v1/auth/register", map[string]string{"email": "relay@example.test", "password": "test-password", "name": "Relay"})
			if status != 200 {
				t.Fatalf("register: %d %v", status, out)
			}
			c.token = out["token"].(string)
			dev := dialDevice(t, ctx, base, "nya-revoke", "test-secret", "123456")
			defer dev.conn.CloseNow()
			if status, out = c.do("POST", "/api/v1/devices/claim", map[string]string{"deviceId": "nya-revoke", "claimCode": "123456"}); status != 200 {
				t.Fatalf("claim: %d %v", status, out)
			}
			conn, _, err := websocket.Dial(ctx, base+"/api/v1/relay/nya-revoke?token="+c.token, nil)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.CloseNow()
			_, raw, err := dev.conn.Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			var msg muxMsg
			if json.Unmarshal(raw, &msg) != nil || msg.Op != "open" {
				t.Fatalf("expected open: %s", raw)
			}
			switch action {
			case "device-close":
				dev.write(t, muxMsg{Op: "close", Sid: msg.Sid})
				status = 200
			case "logout":
				status, out = c.do("POST", "/api/v1/auth/logout", nil)
			case "unclaim":
				status, out = c.do("DELETE", "/api/v1/devices/nya-revoke", nil)
			case "offline":
				dev.conn.CloseNow()
				status = 200
			}
			if status != 200 {
				t.Fatalf("%s: %d %v", action, status, out)
			}
			deadline, done := context.WithTimeout(ctx, 2*time.Second)
			defer done()
			_, _, err = conn.Read(deadline)
			if err == nil || deadline.Err() != nil {
				t.Fatalf("idle relay not closed promptly: %v", err)
			}
			if action == "logout" {
				if status, _ := c.do("GET", "/api/v1/auth/me", nil); status != 401 {
					t.Fatalf("revoked token: %d", status)
				}
			}
		})
	}
}

func TestWebsocketOriginLookalike(t *testing.T) {
	st, err := store.Open(filepath.Join(t.TempDir(), "origin.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	sc := stats.New(st)
	reg := relay.New(st, sc)
	srv := httptest.NewServer(New(st, reg, sc, nil))
	defer srv.Close()
	defer reg.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, resp, err := websocket.Dial(ctx, "ws"+strings.TrimPrefix(srv.URL, "http")+"/api/v1/device/connect", &websocket.DialOptions{HTTPHeader: http.Header{"Origin": []string{"http://localhost.evil.test"}}})
	if err == nil || resp == nil || resp.StatusCode != 403 {
		t.Fatalf("origin accepted: %v %v", resp, err)
	}
}
