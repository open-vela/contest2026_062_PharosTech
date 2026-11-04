// SPDX-License-Identifier: Apache-2.0

package nyalink

import (
	"encoding/json"
	"testing"
)

func TestEnvelopeCodec(t *testing.T) {
	raw := []byte(`{"v":1,"id":"c1-42","type":"req","topic":"sys.ping","data":{}}`)
	env, err := DecodeEnvelope(raw)
	if err != nil {
		t.Fatal(err)
	}
	if env.V != 1 || env.ID != "c1-42" || env.Type != "req" || env.Topic != "sys.ping" {
		t.Fatalf("bad decode: %+v", env)
	}

	b, err := EncodeRes("c1-42", "sys.ping", map[string]any{"t": 123})
	if err != nil {
		t.Fatal(err)
	}
	res, err := DecodeEnvelope(b)
	if err != nil {
		t.Fatal(err)
	}
	if res.Type != "res" || res.ID != "c1-42" || res.V != ProtocolVersion {
		t.Fatalf("bad res: %+v", res)
	}

	q, _ := EncodeReq("c9", "sys.hello", map[string]any{"client": "web"})
	qe, _ := DecodeEnvelope(q)
	if qe.Type != "req" || qe.ID != "c9" || qe.Topic != "sys.hello" {
		t.Fatalf("bad req: %+v", qe)
	}

	e, _ := EncodeErr("x", "foo", EAuth, "nope", nil)
	ee, _ := DecodeEnvelope(e)
	var ed ErrData
	if err := json.Unmarshal(ee.Data, &ed); err != nil || ed.Code != EAuth {
		t.Fatalf("bad err data: %+v %v", ed, err)
	}

	ev, _ := EncodeEvent("eye.state", EyeState{Seq: 7})
	eve, _ := DecodeEnvelope(ev)
	if eve.Type != "event" || eve.ID != "" || eve.Topic != "eye.state" {
		t.Fatalf("bad event: %+v", eve)
	}
}

func TestEnums(t *testing.T) {
	if !ValidMode("happy") || ValidMode("nope") {
		t.Fatal("ValidMode broken")
	}
	if !ValidScene("music") || ValidScene("nope") {
		t.Fatal("ValidScene broken")
	}
}
