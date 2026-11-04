// SPDX-License-Identifier: Apache-2.0

package nyalink

// EyeState mirrors the "eye.state" event payload defined in nyalink.md.
type EyeState struct {
	Seq        uint64     `json:"seq"`
	T          int64      `json:"t"` // sender epoch ms
	Expression Expression `json:"expression"`
	Gaze       Gaze       `json:"gaze"`
	Appearance Appearance `json:"appearance"`
	Scene      Scene      `json:"scene"`
	OneShot    []OneShot  `json:"oneShot,omitempty"`
}

// Expression is the current mode block of EyeState.
type Expression struct {
	Mode       string      `json:"mode"`
	Since      int64       `json:"since"`
	LightLevel float64     `json:"lightLevel"`
	SleepLid   *[2]float64 `json:"sleepLid,omitempty"` // only for mode=sleep
}

// Gaze is the gaze block of EyeState.
type Gaze struct {
	Mode      string  `json:"mode"` // "auto" | "target"
	X         float64 `json:"x"`
	Y         float64 `json:"y"`
	HoldUntil int64   `json:"holdUntil"`
}

// Appearance is the iris color block of EyeState.
type Appearance struct {
	IrisLeft  string `json:"irisLeft"`
	IrisRight string `json:"irisRight"`
}

// Scene is the scene block of EyeState; Type null means no scene.
type Scene struct {
	Type    *string        `json:"type"`
	Style   string         `json:"style"`
	Since   int64          `json:"since"`
	Options map[string]any `json:"options"`
	Payload map[string]any `json:"payload"`
}

// OneShot is an instantaneous event (e.g. blink) carried by EyeState.
type OneShot struct {
	Type string `json:"type"`
	At   int64  `json:"at"`
}

// Modes and Scenes enumerations come from app/nyabula_web/packages/eye-params/eye-params.json.
var Modes = []string{
	"idle", "curious", "happy", "processing", "star", "heart", "sleepy",
	"sleep", "angry", "sad", "surprise", "dizzy", "derp",
}

var Scenes = []string{
	"music", "timer", "weather", "battery", "alarm", "call", "task",
	"stopwatch", "calendar", "sleep-timer", "network", "audio", "eq",
	"caption", "briefing", "privacy", "identity", "memory", "devices",
	"system", "health", "presence", "companion", "home", "subwoofer",
	"pairing",
}

// ValidMode reports whether m is a known expression mode.
func ValidMode(m string) bool {
	for _, v := range Modes {
		if v == m {
			return true
		}
	}
	return false
}

// ValidScene reports whether s is a known scene type.
func ValidScene(s string) bool {
	for _, v := range Scenes {
		if v == s {
			return true
		}
	}
	return false
}
