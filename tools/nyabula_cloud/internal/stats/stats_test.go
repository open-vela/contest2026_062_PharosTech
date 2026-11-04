// SPDX-License-Identifier: Apache-2.0

package stats

import (
	"path/filepath"
	"testing"
	"time"

	"nyabula.tech/cloud/internal/store"
)

func TestFlushPreservesFractionalOnlineTime(t *testing.T) {
	st, err := store.Open(filepath.Join(t.TempDir(), "stats.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	if err := st.CreateDevice(&store.Device{DeviceID: "clock-test", SecretHash: "test", ClaimCode: "123456"}); err != nil {
		t.Fatal(err)
	}
	c := New(st)
	start := time.Now().Add(-1500 * time.Millisecond)
	c.get("clock-test").onlineSince = start
	c.Flush()
	if got := c.get("clock-test").onlineSince.Sub(start); got != time.Second {
		t.Fatalf("flush discarded fractional online time: advanced %s", got)
	}
}
