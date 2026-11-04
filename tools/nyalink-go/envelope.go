// SPDX-License-Identifier: Apache-2.0

// Package nyalink holds the protocol-neutral parts of the NyaLink v1
// protocol (see docs/protocol/nyalink.md): the wire envelope, its
// codecs, error codes and the EyeState payload types. Device behaviour
// (hub, demo carousel, servers) lives in the Simulator / Core, not here.
package nyalink

import "encoding/json"

// ProtocolVersion is the only envelope version currently supported.
const ProtocolVersion = 1

// Envelope is the NyaLink wire envelope for all frames.
type Envelope struct {
	V     int             `json:"v"`
	ID    string          `json:"id,omitempty"`
	Type  string          `json:"type"` // "req" | "res" | "event" | "err"
	Topic string          `json:"topic"`
	Data  json.RawMessage `json:"data,omitempty"`
}

// Error codes defined by the protocol (nyalink.md + cloud.md).
const (
	EPerm     = "EPERM"
	EAuth     = "EAUTH"
	EInval    = "EINVAL"
	ENotFound = "ENOTFOUND"
	EBusy     = "EBUSY"
	EOffline  = "EOFFLINE"  // cloud relay: target device not connected
	EConflict = "ECONFLICT" // cloud device link: duplicate registration
)

// ErrData is the payload of a type:"err" envelope.
type ErrData struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	Supported []int  `json:"supported,omitempty"`
}

// DecodeEnvelope parses a raw text frame into an Envelope.
func DecodeEnvelope(raw []byte) (*Envelope, error) {
	var env Envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		return nil, err
	}
	return &env, nil
}

// EncodeReq builds a request envelope.
func EncodeReq(id, topic string, data any) ([]byte, error) {
	return marshalEnv(Envelope{V: ProtocolVersion, ID: id, Type: "req", Topic: topic}, data)
}

// EncodeRes builds a response envelope paired with the request id.
func EncodeRes(id, topic string, data any) ([]byte, error) {
	return marshalEnv(Envelope{V: ProtocolVersion, ID: id, Type: "res", Topic: topic}, data)
}

// EncodeErr builds an error envelope (id may be empty for non-response errors).
func EncodeErr(id, topic, code, message string, supported []int) ([]byte, error) {
	return marshalEnv(Envelope{V: ProtocolVersion, ID: id, Type: "err", Topic: topic},
		ErrData{Code: code, Message: message, Supported: supported})
}

// EncodeEvent builds an event envelope (no id per protocol).
func EncodeEvent(topic string, data any) ([]byte, error) {
	return marshalEnv(Envelope{V: ProtocolVersion, Type: "event", Topic: topic}, data)
}

func marshalEnv(env Envelope, data any) ([]byte, error) {
	b, err := json.Marshal(data)
	if err != nil {
		return nil, err
	}
	env.Data = b
	return json.Marshal(env)
}
