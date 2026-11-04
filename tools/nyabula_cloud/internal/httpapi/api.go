// SPDX-License-Identifier: Apache-2.0

// Package httpapi wires the nyacloud HTTP surface: /api/v1 REST endpoints
// plus the two WebSocket endpoints (device connect, client relay).
package httpapi

import (
	"encoding/json"
	"net/http"
	"net/url"
	"strings"
	"time"

	"nyabula.tech/cloud/internal/auth"
	"nyabula.tech/cloud/internal/relay"
	"nyabula.tech/cloud/internal/stats"
	"nyabula.tech/cloud/internal/store"
	ny "nyabula.tech/nyalink"
)

// API bundles the service dependencies.
type API struct {
	st      *store.Store
	reg     *relay.Registry
	stats   *stats.Collector
	origins []string // extra allowed CORS origins
}

// New builds the full HTTP handler.
func New(st *store.Store, reg *relay.Registry, sc *stats.Collector, corsOrigins []string) http.Handler {
	a := &API{st: st, reg: reg, stats: sc, origins: corsOrigins}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})

	mux.HandleFunc("POST /api/v1/auth/register", a.handleRegister)
	mux.HandleFunc("POST /api/v1/auth/login", a.handleLogin)
	mux.HandleFunc("POST /api/v1/auth/logout", a.withUser(a.handleLogout))
	mux.HandleFunc("GET /api/v1/auth/me", a.withUser(a.handleMe))
	mux.HandleFunc("GET /api/v1/devices", a.withUser(a.handleDevices))
	mux.HandleFunc("POST /api/v1/devices/claim", a.withUser(a.handleClaim))
	mux.HandleFunc("DELETE /api/v1/devices/{deviceId}", a.withUser(a.handleUnclaim))
	mux.HandleFunc("GET /api/v1/devices/{deviceId}/stats", a.withUser(a.handleDeviceStats))
	mux.HandleFunc("GET /api/v1/stats/overview", a.withUser(a.handleOverview))
	mux.HandleFunc("GET /api/v1/device/connect", a.websocketOrigin(reg.HandleDeviceConnect))
	mux.HandleFunc("GET /api/v1/relay/{deviceId}", a.handleRelay)

	return a.cors(mux)
}

// --- plumbing ------------------------------------------------------------

func (a *API) cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origin := r.Header.Get("Origin")
		if origin != "" && a.originAllowed(origin) {
			w.Header().Set("Access-Control-Allow-Origin", origin)
			w.Header().Set("Access-Control-Allow-Headers", "Authorization, Content-Type")
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
			w.Header().Set("Vary", "Origin")
		}
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// originAllowed accepts local dev origins by default plus configured ones.
func (a *API) originAllowed(origin string) bool {
	for _, o := range a.origins {
		if o == "*" || strings.EqualFold(o, origin) {
			return true
		}
	}
	u, err := url.Parse(origin)
	return err == nil && u.Scheme == "http" && u.User == nil && u.Path == "" &&
		(u.Hostname() == "localhost" || u.Hostname() == "127.0.0.1" || u.Hostname() == "::1")
}

func (a *API) websocketOrigin(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		origin := r.Header.Get("Origin")
		if origin != "" {
			u, err := url.Parse(origin)
			if err != nil || (u.Host != r.Host && !a.originAllowed(origin)) {
				writeErr(w, http.StatusForbidden, ny.EPerm, "origin not allowed")
				return
			}
		}
		next(w, r)
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// writeErr emits the unified {error:{code,message}} shape.
func writeErr(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]any{
		"error": map[string]string{"code": code, "message": msg},
	})
}

func decodeBody(w http.ResponseWriter, r *http.Request, v any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, 16*1024)
	if err := json.NewDecoder(r.Body).Decode(v); err != nil {
		writeErr(w, http.StatusBadRequest, ny.EInval, "bad JSON body")
		return false
	}
	return true
}

// bearerUser resolves the Authorization: Bearer token to a user.
func (a *API) bearerUser(r *http.Request) *store.User {
	h := r.Header.Get("Authorization")
	tok, ok := strings.CutPrefix(h, "Bearer ")
	if !ok || tok == "" {
		return nil
	}
	u, err := a.st.SessionUser(tok)
	if err != nil {
		return nil
	}
	return u
}

func (a *API) withUser(fn func(http.ResponseWriter, *http.Request, *store.User)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		u := a.bearerUser(r)
		if u == nil {
			writeErr(w, http.StatusUnauthorized, ny.EAuth, "missing or invalid token")
			return
		}
		fn(w, r, u)
	}
}

func userJSON(u *store.User) map[string]any {
	return map[string]any{"id": u.ID, "email": u.Email, "name": u.Name}
}

// --- auth ----------------------------------------------------------------

func (a *API) handleRegister(w http.ResponseWriter, r *http.Request) {
	var d struct{ Email, Password, Name string }
	if !decodeBody(w, r, &d) {
		return
	}
	if d.Email == "" || len(d.Password) < 6 {
		writeErr(w, http.StatusBadRequest, ny.EInval, "email and password (>=6 chars) required")
		return
	}
	if _, err := a.st.UserByEmail(d.Email); err == nil {
		writeErr(w, http.StatusConflict, ny.EConflict, "email already registered")
		return
	}
	hash, err := auth.HashPassword(d.Password)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "hash failure")
		return
	}
	u, err := a.st.CreateUser(d.Email, d.Name, hash)
	if err != nil {
		writeErr(w, http.StatusConflict, ny.EConflict, "email already registered")
		return
	}
	a.issueToken(w, u)
}

func (a *API) handleLogin(w http.ResponseWriter, r *http.Request) {
	var d struct{ Email, Password string }
	if !decodeBody(w, r, &d) {
		return
	}
	u, err := a.st.UserByEmail(d.Email)
	if err != nil || !auth.VerifyPassword(u.PassHash, d.Password) {
		writeErr(w, http.StatusUnauthorized, ny.EAuth, "bad email or password")
		return
	}
	a.issueToken(w, u)
}

func (a *API) issueToken(w http.ResponseWriter, u *store.User) {
	tok := auth.NewToken()
	if err := a.st.CreateSession(tok, u.ID); err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "session store failure")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"token": tok, "user": userJSON(u)})
}

func (a *API) handleMe(w http.ResponseWriter, _ *http.Request, u *store.User) {
	writeJSON(w, http.StatusOK, map[string]any{"user": userJSON(u)})
}

func (a *API) handleLogout(w http.ResponseWriter, r *http.Request, u *store.User) {
	token := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	if err := a.st.DeleteSession(token); err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "session store failure")
		return
	}
	a.reg.Revoke(u.ID, "", token)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// --- devices -------------------------------------------------------------

func (a *API) handleDevices(w http.ResponseWriter, _ *http.Request, u *store.User) {
	devs, err := a.st.DevicesByOwner(u.ID)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "store failure")
		return
	}
	out := []map[string]any{}
	for _, d := range devs {
		out = append(out, a.deviceJSON(d))
	}
	writeJSON(w, http.StatusOK, map[string]any{"devices": out})
}

func (a *API) deviceJSON(d *store.Device) map[string]any {
	online, _ := a.reg.Online(d.DeviceID)
	return map[string]any{
		"deviceId":    d.DeviceID,
		"name":        d.Name,
		"online":      online,
		"claimedAt":   d.ClaimedAt.Format(time.RFC3339),
		"coreVersion": d.CoreVersion,
		"lastSeen":    d.LastSeen.Format(time.RFC3339),
	}
}

func (a *API) handleClaim(w http.ResponseWriter, r *http.Request, u *store.User) {
	var d struct{ DeviceID, ClaimCode string }
	if !decodeBody(w, r, &d) {
		return
	}
	dev, err := a.st.DeviceByID(d.DeviceID)
	if err != nil {
		writeErr(w, http.StatusNotFound, ny.ENotFound, "unknown device")
		return
	}
	if dev.OwnerID != 0 {
		writeErr(w, http.StatusConflict, ny.EConflict, "device already claimed")
		return
	}
	if dev.ClaimCode == "" || dev.ClaimCode != d.ClaimCode {
		writeErr(w, http.StatusForbidden, ny.EPerm, "wrong claim code")
		return
	}
	if err := a.st.ClaimDevice(d.DeviceID, u.ID); err != nil {
		writeErr(w, http.StatusConflict, ny.EConflict, "claim race lost")
		return
	}
	dev, _ = a.st.DeviceByID(d.DeviceID)
	writeJSON(w, http.StatusOK, map[string]any{"device": a.deviceJSON(dev)})
}

func (a *API) handleUnclaim(w http.ResponseWriter, r *http.Request, u *store.User) {
	id := r.PathValue("deviceId")
	if err := a.st.UnclaimDevice(id, u.ID); err != nil {
		writeErr(w, http.StatusNotFound, ny.ENotFound, "device not owned by you")
		return
	}
	a.reg.Revoke(u.ID, id, "")
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (a *API) ownedDevice(w http.ResponseWriter, u *store.User, id string) *store.Device {
	dev, err := a.st.DeviceByID(id)
	if err != nil {
		writeErr(w, http.StatusNotFound, ny.ENotFound, "unknown device")
		return nil
	}
	if dev.OwnerID != u.ID {
		writeErr(w, http.StatusForbidden, ny.EPerm, "device not owned by you")
		return nil
	}
	return dev
}

func (a *API) handleDeviceStats(w http.ResponseWriter, r *http.Request, u *store.User) {
	dev := a.ownedDevice(w, u, r.PathValue("deviceId"))
	if dev == nil {
		return
	}
	a.stats.Flush() // fold live counters in before reading
	daily, err := a.st.DailyStats(dev.DeviceID)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "store failure")
		return
	}
	online, clients := a.reg.Online(dev.DeviceID)
	writeJSON(w, http.StatusOK, map[string]any{
		"daily":   daily,
		"current": map[string]any{"online": online, "clients": clients},
	})
}

func (a *API) handleOverview(w http.ResponseWriter, _ *http.Request, u *store.User) {
	devs, err := a.st.DevicesByOwner(u.ID)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "store failure")
		return
	}
	ids := make([]string, 0, len(devs))
	for _, d := range devs {
		ids = append(ids, d.DeviceID)
	}
	a.stats.Flush()
	frames, err := a.st.FramesToday(ids, time.Now().Format("2006-01-02"))
	if err != nil {
		writeErr(w, http.StatusInternalServerError, ny.EBusy, "store failure")
		return
	}
	online, clients := a.reg.TotalClients(ids)
	writeJSON(w, http.StatusOK, map[string]any{
		"devices": len(devs), "online": online,
		"clients": clients, "framesToday": frames,
	})
}

// --- relay ---------------------------------------------------------------

// handleRelay authenticates via ?token= (browser WS cannot set headers)
// and checks ownership before handing over to the registry bridge.
func (a *API) handleRelay(w http.ResponseWriter, r *http.Request) {
	tok := r.URL.Query().Get("token")
	if tok == "" {
		writeErr(w, http.StatusUnauthorized, ny.EAuth, "token query parameter required")
		return
	}
	u, err := a.st.SessionUser(tok)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, ny.EAuth, "invalid token")
		return
	}
	id := r.PathValue("deviceId")
	dev, err := a.st.DeviceByID(id)
	if err != nil {
		writeErr(w, http.StatusNotFound, ny.ENotFound, "unknown device")
		return
	}
	if dev.OwnerID != u.ID {
		writeErr(w, http.StatusForbidden, ny.EPerm, "device not owned by you")
		return
	}
	a.websocketOrigin(func(w http.ResponseWriter, r *http.Request) {
		a.reg.HandleRelay(w, r, id, u.ID, tok)
	})(w, r)
}
