// SPDX-License-Identifier: Apache-2.0

// Package store is the SQLite persistence layer of nyacloud:
// users, sessions, devices and per-day device stats.
package store

import (
	"database/sql"
	"errors"
	"time"

	_ "modernc.org/sqlite" // pure-Go sqlite driver, no cgo
)

// ErrNotFound is returned when a row does not exist.
var ErrNotFound = errors.New("not found")

// User is one account row.
type User struct {
	ID        int64
	Email     string
	Name      string
	PassHash  string
	CreatedAt time.Time
}

// Device is one device row. OwnerID is 0 while unclaimed.
type Device struct {
	DeviceID    string
	Name        string
	SecretHash  string
	ClaimCode   string
	OwnerID     int64
	ClaimedAt   time.Time
	CoreVersion string
	LastSeen    time.Time
}

// DailyStat is one aggregated stats row.
type DailyStat struct {
	Date          string `json:"date"` // YYYY-MM-DD
	OnlineSeconds int64  `json:"onlineSeconds"`
	FramesUp      int64  `json:"framesUp"`
	FramesDown    int64  `json:"framesDown"`
}

// Store wraps the SQLite database.
type Store struct{ db *sql.DB }

// Open opens (and migrates) the database at path.
func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path+"?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, err
	}
	// SQLite allows one writer; serialize all access through one conn.
	db.SetMaxOpenConns(1)
	s := &Store{db: db}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

// Close closes the database.
func (s *Store) Close() error { return s.db.Close() }

func (s *Store) migrate() error {
	_, err := s.db.Exec(`
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  pass_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id),
  expires_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  secret_hash TEXT NOT NULL,
  claim_code TEXT NOT NULL,
  owner_id INTEGER NOT NULL DEFAULT 0,
  claimed_at INTEGER NOT NULL DEFAULT 0,
  core_version TEXT NOT NULL DEFAULT '',
  last_seen INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS device_stats_daily (
  device_id TEXT NOT NULL,
  date TEXT NOT NULL,
  online_seconds INTEGER NOT NULL DEFAULT 0,
  frames_up INTEGER NOT NULL DEFAULT 0,
  frames_down INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (device_id, date)
);`)
	return err
}

// --- users ---------------------------------------------------------------

// CreateUser inserts an account; returns ErrConflict-ish error on dup email.
func (s *Store) CreateUser(email, name, passHash string) (*User, error) {
	res, err := s.db.Exec(`INSERT INTO users(email,name,pass_hash,created_at) VALUES(?,?,?,?)`,
		email, name, passHash, time.Now().Unix())
	if err != nil {
		return nil, err
	}
	id, _ := res.LastInsertId()
	return &User{ID: id, Email: email, Name: name, PassHash: passHash}, nil
}

// UserByEmail fetches an account by email.
func (s *Store) UserByEmail(email string) (*User, error) {
	return s.scanUser(s.db.QueryRow(`SELECT id,email,name,pass_hash FROM users WHERE email=?`, email))
}

// UserByID fetches an account by id.
func (s *Store) UserByID(id int64) (*User, error) {
	return s.scanUser(s.db.QueryRow(`SELECT id,email,name,pass_hash FROM users WHERE id=?`, id))
}

func (s *Store) scanUser(row *sql.Row) (*User, error) {
	var u User
	err := row.Scan(&u.ID, &u.Email, &u.Name, &u.PassHash)
	if err == sql.ErrNoRows {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &u, nil
}

// --- sessions ------------------------------------------------------------

// SessionTTL is the sliding token lifetime.
const SessionTTL = 30 * 24 * time.Hour

// CreateSession stores a fresh token for userID.
func (s *Store) CreateSession(token string, userID int64) error {
	_, err := s.db.Exec(`INSERT INTO sessions(token,user_id,expires_at) VALUES(?,?,?)`,
		token, userID, time.Now().Add(SessionTTL).Unix())
	return err
}

// DeleteSession revokes one login token.
func (s *Store) DeleteSession(token string) error {
	_, err := s.db.Exec(`DELETE FROM sessions WHERE token=?`, token)
	return err
}

// SessionUser resolves a token to its user and slides the expiry forward.
func (s *Store) SessionUser(token string) (*User, error) {
	var userID, exp int64
	err := s.db.QueryRow(`SELECT user_id,expires_at FROM sessions WHERE token=?`, token).
		Scan(&userID, &exp)
	if err == sql.ErrNoRows {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	now := time.Now()
	if exp < now.Unix() {
		_, _ = s.db.Exec(`DELETE FROM sessions WHERE token=?`, token)
		return nil, ErrNotFound
	}
	_, _ = s.db.Exec(`UPDATE sessions SET expires_at=? WHERE token=?`,
		now.Add(SessionTTL).Unix(), token)
	return s.UserByID(userID)
}

// --- devices -------------------------------------------------------------

// CreateDevice registers a first-seen device.
func (s *Store) CreateDevice(d *Device) error {
	_, err := s.db.Exec(`INSERT INTO devices(device_id,name,secret_hash,claim_code,core_version,last_seen)
VALUES(?,?,?,?,?,?)`, d.DeviceID, d.Name, d.SecretHash, d.ClaimCode, d.CoreVersion, time.Now().Unix())
	return err
}

// DeviceByID fetches one device.
func (s *Store) DeviceByID(id string) (*Device, error) {
	var d Device
	var claimedAt, lastSeen int64
	err := s.db.QueryRow(`SELECT device_id,name,secret_hash,claim_code,owner_id,claimed_at,core_version,last_seen
FROM devices WHERE device_id=?`, id).
		Scan(&d.DeviceID, &d.Name, &d.SecretHash, &d.ClaimCode, &d.OwnerID, &claimedAt, &d.CoreVersion, &lastSeen)
	if err == sql.ErrNoRows {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	d.ClaimedAt = time.Unix(claimedAt, 0)
	d.LastSeen = time.Unix(lastSeen, 0)
	return &d, nil
}

// TouchDevice updates name/coreVersion/lastSeen on (re)connect or disconnect.
func (s *Store) TouchDevice(id, name, coreVersion string) error {
	_, err := s.db.Exec(`UPDATE devices SET name=?,core_version=?,last_seen=? WHERE device_id=?`,
		name, coreVersion, time.Now().Unix(), id)
	return err
}

// ClaimDevice binds an unclaimed device to userID.
func (s *Store) ClaimDevice(id string, userID int64) error {
	res, err := s.db.Exec(`UPDATE devices SET owner_id=?,claimed_at=? WHERE device_id=? AND owner_id=0`,
		userID, time.Now().Unix(), id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// UnclaimDevice releases a device owned by userID.
func (s *Store) UnclaimDevice(id string, userID int64) error {
	res, err := s.db.Exec(`UPDATE devices SET owner_id=0,claimed_at=0 WHERE device_id=? AND owner_id=?`,
		id, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// DevicesByOwner lists devices claimed by userID.
func (s *Store) DevicesByOwner(userID int64) ([]*Device, error) {
	rows, err := s.db.Query(`SELECT device_id,name,secret_hash,claim_code,owner_id,claimed_at,core_version,last_seen
FROM devices WHERE owner_id=? ORDER BY device_id`, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*Device
	for rows.Next() {
		var d Device
		var claimedAt, lastSeen int64
		if err := rows.Scan(&d.DeviceID, &d.Name, &d.SecretHash, &d.ClaimCode, &d.OwnerID,
			&claimedAt, &d.CoreVersion, &lastSeen); err != nil {
			return nil, err
		}
		d.ClaimedAt = time.Unix(claimedAt, 0)
		d.LastSeen = time.Unix(lastSeen, 0)
		out = append(out, &d)
	}
	return out, rows.Err()
}

// --- stats ---------------------------------------------------------------

// AddDailyStats accumulates one flush into the per-day row (upsert-add).
func (s *Store) AddDailyStats(deviceID, date string, onlineSecs, up, down int64) error {
	_, err := s.db.Exec(`INSERT INTO device_stats_daily(device_id,date,online_seconds,frames_up,frames_down)
VALUES(?,?,?,?,?)
ON CONFLICT(device_id,date) DO UPDATE SET
  online_seconds=online_seconds+excluded.online_seconds,
  frames_up=frames_up+excluded.frames_up,
  frames_down=frames_down+excluded.frames_down`,
		deviceID, date, onlineSecs, up, down)
	return err
}

// DailyStats returns the aggregated rows for one device, newest first.
func (s *Store) DailyStats(deviceID string) ([]DailyStat, error) {
	rows, err := s.db.Query(`SELECT date,online_seconds,frames_up,frames_down
FROM device_stats_daily WHERE device_id=? ORDER BY date DESC LIMIT 90`, deviceID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []DailyStat{}
	for rows.Next() {
		var d DailyStat
		if err := rows.Scan(&d.Date, &d.OnlineSeconds, &d.FramesUp, &d.FramesDown); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}

// FramesToday sums today's up+down frames across the given devices.
func (s *Store) FramesToday(deviceIDs []string, date string) (int64, error) {
	var total int64
	for _, id := range deviceIDs {
		var up, down int64
		err := s.db.QueryRow(`SELECT frames_up,frames_down FROM device_stats_daily
WHERE device_id=? AND date=?`, id, date).Scan(&up, &down)
		if err == sql.ErrNoRows {
			continue
		}
		if err != nil {
			return 0, err
		}
		total += up + down
	}
	return total, nil
}
