// SPDX-License-Identifier: Apache-2.0

// Package stats accumulates per-device usage counters in memory and
// flushes them into device_stats_daily. Frame contents are never stored.
package stats

import (
	"context"
	"log"
	"sync"
	"time"

	"nyabula.tech/cloud/internal/store"
)

type acc struct {
	onlineSince time.Time // zero when offline
	up, down    int64
}

// Collector aggregates online seconds and frame counts per device.
type Collector struct {
	st *store.Store
	mu sync.Mutex
	m  map[string]*acc
}

// New creates a Collector backed by st.
func New(st *store.Store) *Collector {
	return &Collector{st: st, m: map[string]*acc{}}
}

func (c *Collector) get(id string) *acc {
	a := c.m[id]
	if a == nil {
		a = &acc{}
		c.m[id] = a
	}
	return a
}

// DeviceOnline marks the start of an online span.
func (c *Collector) DeviceOnline(id string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.get(id).onlineSince = time.Now()
}

// DeviceOffline ends the online span and flushes the device row.
func (c *Collector) DeviceOffline(id string) {
	c.flush(id, true)
}

// FrameUp counts one device->client frame.
func (c *Collector) FrameUp(id string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.get(id).up++
}

// FrameDown counts one client->device frame.
func (c *Collector) FrameDown(id string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.get(id).down++
}

// Flush writes all accumulated counters to the store.
func (c *Collector) Flush() {
	c.mu.Lock()
	ids := make([]string, 0, len(c.m))
	for id := range c.m {
		ids = append(ids, id)
	}
	c.mu.Unlock()
	for _, id := range ids {
		c.flush(id, false)
	}
}

func (c *Collector) flush(id string, goingOffline bool) {
	now := time.Now()
	c.mu.Lock()
	a := c.m[id]
	if a == nil {
		c.mu.Unlock()
		return
	}
	var secs int64
	if !a.onlineSince.IsZero() {
		secs = int64(now.Sub(a.onlineSince).Seconds())
		if goingOffline {
			a.onlineSince = time.Time{}
		} else {
			// Preserve fractional seconds when dashboards flush frequently.
			a.onlineSince = a.onlineSince.Add(time.Duration(secs) * time.Second)
		}
	}
	up, down := a.up, a.down
	a.up, a.down = 0, 0
	if goingOffline && a.onlineSince.IsZero() && up == 0 && down == 0 && secs == 0 {
		delete(c.m, id)
	}
	c.mu.Unlock()
	if secs == 0 && up == 0 && down == 0 {
		return
	}
	date := now.Format("2006-01-02")
	if err := c.st.AddDailyStats(id, date, secs, up, down); err != nil {
		log.Printf("stats flush %s: %v", id, err)
	}
}

// Run flushes periodically until ctx is done, then flushes once more.
func (c *Collector) Run(ctx context.Context) {
	t := time.NewTicker(time.Minute)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			c.Flush()
			return
		case <-t.C:
			c.Flush()
		}
	}
}
