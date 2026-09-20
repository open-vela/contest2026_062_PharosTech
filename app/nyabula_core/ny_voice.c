/****************************************************************************
 * app/nyabula_core/ny_voice.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * The voice service.  Who does what is in ny_voice.h; this is WHY it is
 * arranged the way it is.
 *
 * One codec, one clock.  The ES8388 driver refuses to configure one
 * direction in a rate, width or channel count other than the one the other
 * direction is reserved in (es8388_check_peer_format).  The microphone wants
 * 16 kHz mono, the speaker gets 44.1 kHz from the TTS model and from every
 * other player in the product.  Three arrangements are offered:
 *
 *   HALF        (default) the microphone is closed while the robot speaks.
 *               It is the only one built on what has been measured on the
 *               board (16 kHz mono capture, 44.1 kHz playback), and it needs
 *               no echo handling: the robot cannot wake itself.  Barge-in is
 *               then the panel's voice.listen / voice.cancel, not the voice.
 *   SHARED_16K  speech is resampled to 16 kHz mono, so both directions run
 *               in the capture format and the microphone stays open.
 *   SHARED_44K  the microphone runs in the speaker's format (44.1 kHz
 *               stereo) and is decimated to 16 kHz here, so the flash
 *               player, the alarm chime and Bluetooth music coexist with
 *               the wake word without any arbitration.
 *
 * In HALF and SHARED_16K another player that opens the speaker in its own
 * format fails with -EBUSY for as long as the microphone is open; it has to
 * call ny_voice_speaker_claim() first, as the flash player already does for
 * Bluetooth.
 *
 * Echo.  There is no echo canceller.  While the robot speaks the microphone
 * is not streamed to the wake word service -- always in HALF, and in the
 * SHARED arrangements unless NYABULA_CORE_VOICE_BARGE_IN says the owner
 * prefers interrupting by voice to the risk of a false wake; even then a
 * reply that itself contains the wake word is spoken deaf.
 *
 * The owner.  A voice turn is submitted to the agent as the owner, because
 * the robot has one owner and no way (yet) to tell voices apart.  What keeps
 * that safe is the agent's own rule: an action with side effects waits for
 * an approval given on the panel, and this service only ever ASKS for it.
 *
 * Threads never share a compute port: a port delivers the frames of the
 * request it last sent, and every message of one speech request carries the
 * BEGIN's id, so KWS belongs to the pump, ASR to the turn thread and TTS to
 * the player.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NYABULA_CORE_VOICE

#include <nuttx/mutex.h>

#include <arch/chip/rk3576_shmem.h>
#include <arch/chip/rk3576_shmem_layout.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ny_compute.h"
#include "ny_product.h"
#include "ny_product_store.h"
#include "ny_voice.h"
#include "ny_voice_audio.h"
#include "ny_voice_dsp.h"
#include "ny_voice_sm.h"
#include "ny_voice_wire.h"
#include "nyamp_protocol.h"

#ifdef CONFIG_NYABULA_CORE_BT
#include "ny_product_bt.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NYABULA_CORE_VOICE_PRIORITY
#define CONFIG_NYABULA_CORE_VOICE_PRIORITY 100
#endif

#ifndef CONFIG_NYABULA_CORE_VOICE_STACKSIZE
#define CONFIG_NYABULA_CORE_VOICE_STACKSIZE 32768
#endif

#ifndef CONFIG_NYABULA_CORE_VOICE_WINDOW_MS
#define CONFIG_NYABULA_CORE_VOICE_WINDOW_MS 200
#endif

/* Well under half of NYABULA_EYE_DEFAULT_LEASE_MS (5 s). */

#define NY_VOICE_EYES_RENEW_S 2

#ifndef CONFIG_NYABULA_CORE_VOICE_VAD_MIN_RMS
#define CONFIG_NYABULA_CORE_VOICE_VAD_MIN_RMS 150
#endif

#ifndef CONFIG_NYABULA_CORE_VOICE_SPEAKER
#define CONFIG_NYABULA_CORE_VOICE_SPEAKER 1
#endif

#ifndef CONFIG_NYABULA_CORE_VOICE_CUE_DIR
#define CONFIG_NYABULA_CORE_VOICE_CUE_DIR "/data/voice"
#endif

#ifndef CONFIG_NYABULA_CORE_VOICE_UNLOAD_IDLE_S
#define CONFIG_NYABULA_CORE_VOICE_UNLOAD_IDLE_S 0
#endif

/* The codec arrangement; see the top of this file. */

#if defined(CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_44K)
#define NY_VOICE_DUPLEX       true
#define NY_VOICE_MODE         "shared-44k"
#define NY_VOICE_MIC_RATE     44100U
#define NY_VOICE_MIC_CHANNELS 2
#define NY_VOICE_OUT_RATE     44100U
#define NY_VOICE_OUT_CHANNELS 2
#elif defined(CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_16K)
#define NY_VOICE_DUPLEX       true
#define NY_VOICE_MODE         "shared-16k"
#define NY_VOICE_MIC_RATE     16000U
#define NY_VOICE_MIC_CHANNELS 1
#define NY_VOICE_OUT_RATE     16000U
#define NY_VOICE_OUT_CHANNELS 1
#else
#define NY_VOICE_DUPLEX       false
#define NY_VOICE_MODE         "half"
#define NY_VOICE_MIC_RATE     16000U
#define NY_VOICE_MIC_CHANNELS 1
#define NY_VOICE_OUT_RATE     44100U
#define NY_VOICE_OUT_CHANNELS 2
#endif

#ifdef CONFIG_NYABULA_CORE_VOICE_BARGE_IN
#define NY_VOICE_BARGE_IN true
#else
#define NY_VOICE_BARGE_IN false
#endif

#ifdef CONFIG_NYABULA_CORE_VOICE_WIRE_S16
#define NY_VOICE_WIRE_S16 true
#else
#define NY_VOICE_WIRE_S16 false
#endif

#define NY_VOICE_STORE  "voice"
#define NY_VOICE_CALLER "voice"

/* Capture: 100 ms chunks into a 4 s ring.  The ring is longer than any
 * wake phrase plus the gate's reaction, so that a gate which opened late
 * still delivers the start of the phrase.
 */

#define NY_VOICE_CHUNK_SAMPLES   1600U
#define NY_VOICE_CHUNK_MS        100U
#define NY_VOICE_RING_SAMPLES    (4U * NY_VOICE_CAPTURE_RATE)
#define NY_VOICE_PREROLL_SAMPLES (3U * NY_VOICE_CAPTURE_RATE / 2U)
#define NY_VOICE_MIC_FRAMES      (NY_VOICE_MIC_RATE / 10U)

#define NY_VOICE_WINDOW_SAMPLES \
  (CONFIG_NYABULA_CORE_VOICE_WINDOW_MS * NY_VOICE_CAPTURE_RATE / 1000U)

/* Half a second per TTS window: the first one is heard sooner, and the
 * window is released as soon as it is copied, so the compute domain
 * synthesizes the next while this one plays.
 */

#define NY_VOICE_TTS_WINDOW     22050U
#define NY_VOICE_OUT_CHUNK      8192U

#define NY_VOICE_SENTENCE_MAX   400U
#define NY_VOICE_TEXT_MAX       2048U
#define NY_VOICE_REPLY_MAX      8192U
#define NY_VOICE_TRANSCRIPT_MAX 512U
#define NY_VOICE_ERROR_MAX      96U
#define NY_VOICE_EVENTS         32U

#define NY_VOICE_LOAD_MS        120000
#define NY_VOICE_TTS_LOAD_MS    300000
#define NY_VOICE_RETRY_MS       5000U
#define NY_VOICE_AGENT_POLL_MS  300U
#define NY_VOICE_AGENT_RETRY_MS 10000U
#define NY_VOICE_SESSION_MS     300000U
#define NY_VOICE_CLAIM_MS       1500U

#define NY_VOICE_CUE_NOT_HEARD                   \
  "\xe6\xb2\xa1\xe5\x90\xac\xe6\xb8\x85\xef\xbc" \
  "\x8c\xe5\x86\x8d\xe8\xaf\xb4\xe4\xb8\x80"     \
  "\xe9\x81\x8d\xe5\x90\xa7\xe3\x80\x82"
#define NY_VOICE_CUE_APPROVAL                    \
  "\xe8\xbf\x99\xe4\xb8\xaa\xe6\x93\x8d\xe4\xbd" \
  "\x9c\xe9\x9c\x80\xe8\xa6\x81\xe4\xbd\xa0"     \
  "\xe5\x9c\xa8\xe9\x9d\xa2\xe6\x9d\xbf\xe4"     \
  "\xb8\x8a\xe7\xa1\xae\xe8\xae\xa4\xe4\xb8"     \
  "\x80\xe4\xb8\x8b\xe3\x80\x82"
#define NY_VOICE_CUE_BUSY                        \
  "\xe6\x88\x91\xe6\xad\xa3\xe5\x9c\xa8\xe5\xbf" \
  "\x99\xef\xbc\x8c\xe7\xa8\x8d\xe7\xad\x89"     \
  "\xe4\xb8\x80\xe4\xb8\x8b\xe3\x80\x82"
#define NY_VOICE_CUE_ERROR                       \
  "\xe5\x87\xba\xe4\xba\x86\xe7\x82\xb9\xe9\x97" \
  "\xae\xe9\xa2\x98\xef\xbc\x8c\xe7\xa8\x8d"     \
  "\xe5\x90\x8e\xe5\x86\x8d\xe8\xaf\x95\xe3"     \
  "\x80\x82"
#define NY_VOICE_CUE_WAIT "\xe7\xa8\x8d\xe7\xad\x89\xe3\x80\x82"

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_voice_model_state_e
{
  NY_VOICE_MODEL_UNLOADED = 0,
  NY_VOICE_MODEL_LOADING,
  NY_VOICE_MODEL_READY,
  NY_VOICE_MODEL_ERROR
};

/* What is spoken.  The cues beyond the state machine's are this file's. */

enum ny_voice_cue_e
{
  NY_VOICE_CUE_NONE = 0, /* Text through TTS.                 */
  NY_VOICE_CUE_SHORT,    /* A cue: never worth a model load.  */
  NY_VOICE_CUE_FIXED     /* A fixed sentence that must be said. */
};

struct ny_voice_model_s
{
  enum ny_voice_model_state_e state;
  int error;
  uint64_t done; /* Pull progress of the load in flight. */
  uint64_t total;
};

struct ny_voice_port_s
{
  struct ny_compute_port_s *port;
  struct ny_voice_wire_s wire;
  struct ny_voice_model_s *loading; /* Where pull progress goes. */
  bool player;                      /* The player's: a cancelled job ends */
                                    /* a wait; else a queued cancel does. */
};

struct ny_voice_queued_s
{
  enum ny_voice_event_e type;
  bool ok;
  bool empty;
  uint32_t job;
};

struct ny_voice_settings_s
{
  bool enabled;
  int wake_sensitivity; /* 0..100, 50 = the evaluated default.   */
  uint32_t max_listen_ms;
  uint32_t reply_voice; /* TTS speaker id.                       */
  uint32_t reply_speed; /* Percent: 50..200.                     */
};

struct ny_voice_s
{
  mutex_t lock;      /* Everything below that is not a thread's own. */
  mutex_t ring_lock; /* The ring and the capture facts beside it.    */

  bool running;
  bool stopping;
  bool loaded; /* Settings were read from the store. */
  struct ny_voice_settings_s settings;

  sem_t events_ready;
  struct ny_voice_queued_s events[NY_VOICE_EVENTS];
  unsigned int events_head;
  unsigned int events_count;

  enum ny_voice_state_e state; /* Mirror of the machine's, for readers. */

  /* Capture (ring_lock). */

  struct ny_voice_ring_s ring;
  uint64_t last_loud;  /* Position just past the last loud chunk.       */
  uint64_t resumed_at; /* Ring position when the microphone last opened. */
  bool capture_gap;    /* It was closed since the last pushed window.    */
  bool gate_open;
  uint32_t level; /* RMS of the last chunk.                         */
  sem_t captured;

  bool capture_open;
  bool capture_hold;   /* Our own playback needs the codec (HALF).       */
  unsigned int claims; /* Other players that asked for it.              */
  int capture_error;

  /* Pump. */

  bool streaming;  /* A KWS stream is open.  */
  bool kws_reload; /* The sensitivity changed. */
  uint64_t stream_next;
  uint64_t stream_base;
  uint32_t windows;
  uint32_t detections;
  struct nyamp_kws_detected_s wake;
  char wake_word[48];

  struct ny_voice_model_s kws;
  struct ny_voice_model_s asr;
  struct ny_voice_model_s tts;

  /* The player's one job. */

  sem_t play_wake;
  bool play_busy;        /* A job is wanted or being played.   */
  uint32_t play_job;     /* The newest job.                    */
  uint32_t play_current; /* The one the player works on.       */
  uint32_t play_dropped; /* Jobs up to this one are cancelled. */
  enum ny_voice_cue_e play_cue;
  const char *play_name; /* The cue's file name, a literal.    */
  bool speaking_wake;    /* What is being said contains the wake word. */
  char play_text[NY_VOICE_REPLY_MAX + 1];

  sem_t eyes_wake;
  int eyes_wanted; /* An ny_voice_eyes_e, or -1. */

  char say_text[NY_VOICE_TEXT_MAX + 1];
  char transcript[NY_VOICE_TRANSCRIPT_MAX];
  uint64_t transcript_ms; /* When the recognizer last changed it.        */
  char reply[NY_VOICE_REPLY_MAX + 1];
  uint32_t turns;
  int last_error;
  char last_error_text[NY_VOICE_ERROR_MAX];
};

/* The turn thread's own. */

struct ny_voice_turn_s
{
  struct ny_voice_sm_s sm;
  struct ny_voice_port_s asr;
  bool asr_open;      /* A request exists on the compute domain.      */
  bool asr_silent;    /* ... that was cancelled: its FINISH is not news. */
  uint64_t asr_start; /* Where it attached.                           */
  char run[64];       /* The agent run being followed, or "".         */
  bool run_pending;   /* It was reported as awaiting approval.        */
  bool submit_due;    /* The agent was busy winding down; try again.  */
  uint64_t submit_until;
  uint64_t poll_at;
  uint64_t last_turn_ms;
  uint64_t idle_since;
  char conversation[40];
  uint32_t generation;
  uint32_t request;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_voice_now_ms(void);
static void ny_voice_error(int code, const char *text);
static void ny_voice_post(enum ny_voice_event_e type, bool ok, bool empty,
                          uint32_t job);
static bool ny_voice_take(struct ny_voice_queued_s *event, int timeout_ms);
static bool ny_voice_stopping(void);
static enum ny_voice_state_e ny_voice_state(void);
static void ny_voice_model_set(struct ny_voice_model_s *model,
                               enum ny_voice_model_state_e state, int error);

static int ny_voice_io_send(void *arg, const uint8_t *wire, size_t size);
static ssize_t ny_voice_io_recv(void *arg, uint8_t *wire, size_t capacity,
                                int timeout_ms);
static uint64_t ny_voice_io_request_id(void *arg);
static uint32_t ny_voice_io_generation(void *arg);
static bool ny_voice_io_interrupted(void *arg);
static void ny_voice_io_progress(void *arg, uint64_t done, uint64_t total,
                                 uint32_t bytes_per_second);
static int ny_voice_port_open(struct ny_voice_port_s *port);
static void ny_voice_port_close(struct ny_voice_port_s *port);
static int ny_voice_model_load(struct ny_voice_port_s *port,
                               struct ny_voice_model_s *model,
                               uint16_t service, const char *directory,
                               int timeout_ms);

static int ny_voice_settings_load(void);
static int ny_voice_settings_save(void);
static float ny_voice_threshold(int sensitivity);

static void *ny_voice_capture_thread(void *arg);
static size_t ny_voice_capture_convert(struct ny_voice_resample_s *resample,
                                       const int16_t *raw, size_t frames,
                                       int16_t *out);
static void *ny_voice_pump_thread(void *arg);
static int ny_voice_pump_begin(struct ny_voice_port_s *port,
                               struct nyamp_buffer_s *grant);
static void ny_voice_pump_events(struct ny_voice_port_s *port, int timeout_ms);
static void *ny_voice_play_thread(void *arg);
static int ny_voice_play_acquire(void);
static void ny_voice_play_release(void);
static size_t ny_voice_play_convert(struct ny_voice_resample_s *resample,
                                    const float *samples, size_t count);
static int ny_voice_play_write(struct ny_voice_audio_s *audio, size_t bytes);
static int ny_voice_play_speech(struct ny_voice_port_s *port,
                                struct ny_voice_audio_s *audio,
                                struct ny_voice_resample_s *resample,
                                const char *text);
static int ny_voice_play_cue(struct ny_voice_audio_s *audio,
                             struct ny_voice_resample_s *resample,
                             const char *text);
static bool ny_voice_play_cancelled(void);
static void *ny_voice_eyes_thread(void *arg);

static int ny_voice_topic(const char *topic, const char *json, cJSON **result);
static void ny_voice_turn_say(const char *text, enum ny_voice_cue_e cue,
                              const char *name);
static void ny_voice_turn_speak(enum ny_voice_speech_e speech);
static void ny_voice_turn_speak_cancel(void);
static void ny_voice_turn_asr_start(struct ny_voice_turn_s *turn, bool attach);
static void ny_voice_turn_asr_poll(struct ny_voice_turn_s *turn);
static void ny_voice_turn_submit(struct ny_voice_turn_s *turn);
static void ny_voice_turn_agent_poll(struct ny_voice_turn_s *turn);
static void ny_voice_turn_act(struct ny_voice_turn_s *turn,
                              const struct ny_voice_sm_result_s *result);
static void ny_voice_turn_idle(struct ny_voice_turn_s *turn);
static int ny_voice_task(int argc, char **argv);
static cJSON *ny_voice_status(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ny_voice_s g_voice =
{
  .lock = NXMUTEX_INITIALIZER,
  .ring_lock = NXMUTEX_INITIALIZER,
  .eyes_wanted = -1,
  .settings =
  {
    .enabled = false,
    .wake_sensitivity = 50,
    .max_listen_ms = 8000,
    .reply_voice = CONFIG_NYABULA_CORE_VOICE_SPEAKER,
    .reply_speed = 100,
  },
};

/* Sample memory.  Static rather than on a stack or the heap: it is needed
 * for as long as the task lives and its size is part of the RAM budget.
 */

static int16_t g_voice_ring[NY_VOICE_RING_SAMPLES];
static int16_t g_voice_mic[NY_VOICE_MIC_FRAMES * NY_VOICE_MIC_CHANNELS];
static int16_t g_voice_chunk[NY_VOICE_CHUNK_SAMPLES + 8];
static int16_t g_voice_window[NYAMP_KWS_WINDOW_SAMPLES_MAX];
static int16_t g_voice_out[NY_VOICE_TTS_WINDOW * 2];
#if defined(CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_44K) || \
    defined(CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_16K)
static float g_voice_float[NY_VOICE_TTS_WINDOW];
static struct ny_voice_resample_s g_voice_resample;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t ny_voice_now_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void ny_voice_error(int code, const char *text)
{
  nxmutex_lock(&g_voice.lock);
  g_voice.last_error = code;
  strlcpy(g_voice.last_error_text, text, sizeof(g_voice.last_error_text));
  nxmutex_unlock(&g_voice.lock);
  if (code != 0)
    {
      syslog(LOG_WARNING, "nyvoice: %s (%d)\n", text, code);
    }
}

static void ny_voice_post(enum ny_voice_event_e type, bool ok, bool empty,
                          uint32_t job)
{
  struct ny_voice_queued_s *slot;

  nxmutex_lock(&g_voice.lock);
  if (g_voice.events_count < NY_VOICE_EVENTS)
    {
      slot = &g_voice.events[(g_voice.events_head + g_voice.events_count) %
                             NY_VOICE_EVENTS];
      slot->type = type;
      slot->ok = ok;
      slot->empty = empty;
      slot->job = job;
      g_voice.events_count++;
      sem_post(&g_voice.events_ready);
    }

  nxmutex_unlock(&g_voice.lock);
}

static bool ny_voice_take(struct ny_voice_queued_s *event, int timeout_ms)
{
  struct timespec until;
  bool taken = false;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += timeout_ms / 1000;
  until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000)
    {
      until.tv_sec++;
      until.tv_nsec -= 1000000000;
    }

  if (sem_timedwait(&g_voice.events_ready, &until) < 0)
    {
      return false;
    }

  nxmutex_lock(&g_voice.lock);
  if (g_voice.events_count != 0)
    {
      *event = g_voice.events[g_voice.events_head];
      g_voice.events_head = (g_voice.events_head + 1) % NY_VOICE_EVENTS;
      g_voice.events_count--;
      taken = true;
    }

  nxmutex_unlock(&g_voice.lock);
  return taken;
}

static bool ny_voice_stopping(void)
{
  bool stopping;

  nxmutex_lock(&g_voice.lock);
  stopping = g_voice.stopping;
  nxmutex_unlock(&g_voice.lock);
  return stopping;
}

static enum ny_voice_state_e ny_voice_state(void)
{
  enum ny_voice_state_e state;

  nxmutex_lock(&g_voice.lock);
  state = g_voice.state;
  nxmutex_unlock(&g_voice.lock);
  return state;
}

static void ny_voice_model_set(struct ny_voice_model_s *model,
                               enum ny_voice_model_state_e state, int error)
{
  nxmutex_lock(&g_voice.lock);
  model->state = state;
  model->error = error;
  if (state != NY_VOICE_MODEL_LOADING)
    {
      model->done = 0;
      model->total = 0;
    }

  nxmutex_unlock(&g_voice.lock);
}

/****************************************************************************
 * The wire's view of a compute port
 ****************************************************************************/

static int ny_voice_io_send(void *arg, const uint8_t *wire, size_t size)
{
  struct ny_voice_port_s *port = arg;

  return port->port == NULL ? -ENOTCONN
                            : ny_compute_port_send(port->port, wire, size);
}

static ssize_t ny_voice_io_recv(void *arg, uint8_t *wire, size_t capacity,
                                int timeout_ms)
{
  struct ny_voice_port_s *port = arg;

  return port->port == NULL
             ? -ENOTCONN
             : ny_compute_port_recv(port->port, wire, capacity, timeout_ms);
}

static uint64_t ny_voice_io_request_id(void *arg)
{
  (void)arg;
  return ny_compute_request_id();
}

static uint32_t ny_voice_io_generation(void *arg)
{
  (void)arg;
  return ny_compute_generation();
}

static bool ny_voice_io_interrupted(void *arg)
{
  struct ny_voice_port_s *port = arg;
  unsigned int index;
  bool interrupted;

  if (port->player)
    {
      return ny_voice_play_cancelled();
    }

  /* The turn thread is inside a model load and cannot take events; a
   * cancel that waits in the queue still has to end the load.
   */

  nxmutex_lock(&g_voice.lock);
  interrupted = g_voice.stopping;
  for (index = 0; index < g_voice.events_count; index++)
    {
      enum ny_voice_event_e type =
          g_voice.events[(g_voice.events_head + index) % NY_VOICE_EVENTS].type;

      interrupted = interrupted || type == NY_VOICE_EV_CANCEL ||
                    type == NY_VOICE_EV_DISABLE;
    }

  nxmutex_unlock(&g_voice.lock);
  return interrupted;
}

static void ny_voice_io_progress(void *arg, uint64_t done, uint64_t total,
                                 uint32_t bytes_per_second)
{
  struct ny_voice_port_s *port = arg;

  (void)bytes_per_second;
  if (port->loading != NULL)
    {
      nxmutex_lock(&g_voice.lock);
      port->loading->done = done;
      port->loading->total = total;
      nxmutex_unlock(&g_voice.lock);
    }
}

/****************************************************************************
 * Name: ny_voice_port_open
 *
 * Description:
 *   Make the conversation usable: a port of the compute service, opened by
 *   the thread that will use it, and the shared arena.  Cheap when both are
 *   already there.
 *
 ****************************************************************************/

static int ny_voice_port_open(struct ny_voice_port_s *port)
{
  struct ny_voice_wire_io_s io;
  int ret;

  if (port->port == NULL)
    {
      ret = ny_compute_port_open(&port->port);
      if (ret < 0)
        {
          return ret;
        }

      memset(&io, 0, sizeof(io));
      io.send = ny_voice_io_send;
      io.recv = ny_voice_io_recv;
      io.request_id = ny_voice_io_request_id;
      io.generation = ny_voice_io_generation;
      io.interrupted = ny_voice_io_interrupted;
      io.progress = ny_voice_io_progress;
      io.arg = port;
      ny_voice_wire_init(&port->wire, &io);
    }

  if (port->wire.io.arena == NULL)
    {
      /* The header may not have been validated yet when the compute domain
       * was slower to claim the region than this domain was to boot.
       */

      if (!rk3576_shmem_ready() && rk3576_shmem_initialize() < 0)
        {
          return -ENODEV;
        }

      port->wire.io.arena = rk3576_shmem_base();
      port->wire.io.arena_size = NYAMP_SHMEM_SIZE;
      port->wire.io.arena_header = NYAMP_SLOT_HEADER + NYAMP_SLOT_HEADER_SIZE;
    }

  return 0;
}

static void ny_voice_port_close(struct ny_voice_port_s *port)
{
  if (port->port != NULL)
    {
      ny_compute_port_close(port->port);
      port->port = NULL;
    }

  port->wire.request_id = 0;
}

static int ny_voice_model_load(struct ny_voice_port_s *port,
                               struct ny_voice_model_s *model,
                               uint16_t service, const char *directory,
                               int timeout_ms)
{
  struct ny_voice_wire_kws_s kws;
  int ret;

  memset(&kws, 0, sizeof(kws));
  nxmutex_lock(&g_voice.lock);
  kws.threshold = ny_voice_threshold(g_voice.settings.wake_sensitivity);
  nxmutex_unlock(&g_voice.lock);

  ny_voice_model_set(model, NY_VOICE_MODEL_LOADING, 0);
  port->loading = model;
  ret = ny_voice_wire_load(&port->wire, service, directory,
                           service == NYAMP_SERVICE_KWS ? &kws : NULL,
                           timeout_ms);
  port->loading = NULL;
  ny_voice_model_set(
      model, ret == 0 ? NY_VOICE_MODEL_READY : NY_VOICE_MODEL_ERROR, ret);
  if (ret < 0)
    {
      char text[NY_VOICE_ERROR_MAX];

      snprintf(text, sizeof(text),
               ret == -ENOENT    ? "/data/models/%s is missing"
               : ret == -ENOTSUP ? "nyampd has no %s backend"
               : ret == -EBUSY   ? "%s: the shared slot is busy"
                                 : "%s model did not load",
               directory);
      ny_voice_error(ret, text);
    }

  return ret;
}

/****************************************************************************
 * Settings
 ****************************************************************************/

static int ny_voice_settings_load(void)
{
  cJSON *root = NULL;
  uint64_t revision;
  int ret;

  nxmutex_lock(&g_voice.lock);
  if (g_voice.loaded)
    {
      nxmutex_unlock(&g_voice.lock);
      return 0;
    }

  nxmutex_unlock(&g_voice.lock);
  ret = ny_product_store_read(NY_VOICE_STORE, &root, &revision);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_voice.lock);
  if (root != NULL)
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
      const cJSON *wake =
          cJSON_GetObjectItemCaseSensitive(root, "wakeSensitivity");
      const cJSON *listen =
          cJSON_GetObjectItemCaseSensitive(root, "maxListenMs");
      const cJSON *who = cJSON_GetObjectItemCaseSensitive(root, "replyVoice");
      const cJSON *speed =
          cJSON_GetObjectItemCaseSensitive(root, "replySpeed");

      /* A field that is absent or out of range keeps its default: the store
       * outlives the firmware that wrote it.
       */

      g_voice.settings.enabled = cJSON_IsTrue(enabled);
      if (cJSON_IsNumber(wake) && wake->valueint >= 0 && wake->valueint <= 100)
        {
          g_voice.settings.wake_sensitivity = wake->valueint;
        }

      if (cJSON_IsNumber(listen) && listen->valueint >= 2000 &&
          listen->valueint <= 30000)
        {
          g_voice.settings.max_listen_ms = (uint32_t)listen->valueint;
        }

      if (cJSON_IsNumber(who) && who->valueint >= 0 && who->valueint <= 255)
        {
          g_voice.settings.reply_voice = (uint32_t)who->valueint;
        }

      if (cJSON_IsNumber(speed) && speed->valueint >= 50 &&
          speed->valueint <= 200)
        {
          g_voice.settings.reply_speed = (uint32_t)speed->valueint;
        }
    }

  g_voice.loaded = true;
  nxmutex_unlock(&g_voice.lock);
  cJSON_Delete(root);
  return 0;
}

static int ny_voice_settings_save(void)
{
  struct ny_voice_settings_s settings;
  cJSON *previous = NULL;
  cJSON *root;
  uint64_t revision;
  bool valid;
  int ret;

  ret = ny_product_store_read(NY_VOICE_STORE, &previous, &revision);
  cJSON_Delete(previous);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_voice.lock);
  settings = g_voice.settings;
  nxmutex_unlock(&g_voice.lock);

  root = cJSON_CreateObject();
  valid = root != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "schema", 1) != NULL;
  valid = valid &&
          cJSON_AddBoolToObject(root, "enabled", settings.enabled) != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "wakeSensitivity",
                                           settings.wake_sensitivity) != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "maxListenMs",
                                           settings.max_listen_ms) != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "replyVoice",
                                           settings.reply_voice) != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "replySpeed",
                                           settings.reply_speed) != NULL;
  ret = valid
            ? ny_product_store_write(NY_VOICE_STORE, root, revision, &revision)
            : -ENOMEM;
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_voice_threshold
 *
 * Description:
 *   The owner's 0..100 as the spotter's keyword threshold.  50 is the value
 *   tools/amp/voice evaluated (0.10); a higher sensitivity is a lower
 *   threshold.  Zero would select the service default, so it is avoided.
 *
 ****************************************************************************/

static float ny_voice_threshold(int sensitivity)
{
  float threshold = 0.20f - 0.002f * (float)sensitivity;

  return threshold < 0.02f ? 0.02f : threshold;
}

/****************************************************************************
 * ny_voice_capture
 ****************************************************************************/

static size_t ny_voice_capture_convert(struct ny_voice_resample_s *resample,
                                       const int16_t *raw, size_t frames,
                                       int16_t *out)
{
#ifdef CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_44K
  static float mono[NY_VOICE_MIC_FRAMES];
  static float slow[NY_VOICE_CHUNK_SAMPLES + 8];
  size_t index;
  size_t count;

  /* Both channels: which of them the microphone is wired to is the
   * board's business, and the other one is silent.
   */

  for (index = 0; index < frames; index++)
    {
      mono[index] =
          ((float)raw[2 * index] + (float)raw[2 * index + 1]) / 32768.0f;
    }

  count = ny_voice_resample(resample, mono, frames, slow,
                            NY_VOICE_CHUNK_SAMPLES + 8);
  ny_voice_f32_to_s16(out, slow, count);
  return count;
#else
  (void)resample;
  memcpy(out, raw, frames * sizeof(int16_t));
  return frames;
#endif
}

static void *ny_voice_capture_thread(void *arg)
{
  static struct ny_voice_gate_config_s gate_config = {
    CONFIG_NYABULA_CORE_VOICE_VAD_MIN_RMS, 40, 2000
  };

  struct ny_voice_resample_s *resample = NULL;
  struct ny_voice_audio_s *audio = NULL;
  struct ny_voice_gate_s gate;
  uint64_t retry_at = 0;

  (void)arg;
#ifdef CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_44K
  resample = &g_voice_resample;
#endif
  ny_voice_gate_init(&gate, &gate_config);

  for (;;)
    {
      struct ny_voice_audio_config_s config;
      size_t bytes = sizeof(g_voice_mic);
      size_t count;
      ssize_t got;
      uint32_t level;
      bool wanted;
      bool loud;

      nxmutex_lock(&g_voice.lock);
      wanted = !g_voice.stopping && g_voice.state != NY_VOICE_OFF &&
               !g_voice.capture_hold && g_voice.claims == 0;
      if (g_voice.stopping)
        {
          nxmutex_unlock(&g_voice.lock);
          break;
        }

      nxmutex_unlock(&g_voice.lock);

      if (!wanted)
        {
          if (audio != NULL)
            {
              ny_voice_audio_close(audio);
              audio = NULL;
            }

          /* The flag is what a claimant and the player wait for. */

          nxmutex_lock(&g_voice.lock);
          g_voice.capture_open = false;
          nxmutex_unlock(&g_voice.lock);
          usleep(20000);
          continue;
        }

      if (audio == NULL)
        {
          int ret;

          if (ny_voice_now_ms() < retry_at)
            {
              usleep(50000);
              continue;
            }

          memset(&config, 0, sizeof(config));
          config.path = CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE;
          config.capture = true;
          config.rate = NY_VOICE_MIC_RATE;
          config.channels = NY_VOICE_MIC_CHANNELS;
          config.chunk = sizeof(g_voice_mic);
          ret = ny_voice_audio_open(&audio, &config);
          nxmutex_lock(&g_voice.lock);
          g_voice.capture_error = ret;
          g_voice.capture_open = ret == 0;
          nxmutex_unlock(&g_voice.lock);
          if (ret < 0)
            {
              /* -EBUSY: someone plays in a format of their own and did not
               * claim the codec.  They were first; try again later.
               */

              audio = NULL;
              retry_at = ny_voice_now_ms() + 1000;
              continue;
            }

          if (resample != NULL)
            {
              ny_voice_resample_init(resample);
            }

          nxmutex_lock(&g_voice.ring_lock);
          g_voice.resumed_at = g_voice.ring.total;
          g_voice.capture_gap = true;
          nxmutex_unlock(&g_voice.ring_lock);
        }

      got = ny_voice_audio_read(audio, g_voice_mic, bytes, 500);
      if (got < 0 || (size_t)got < bytes)
        {
          /* A microphone that stopped delivering is reopened rather than
           * waited for: the wake word depends on it.
           */

          ny_voice_audio_close(audio);
          audio = NULL;
          retry_at = ny_voice_now_ms() + 500;
          nxmutex_lock(&g_voice.lock);
          g_voice.capture_open = false;
          g_voice.capture_error = got < 0 ? (int)got : -ETIMEDOUT;
          nxmutex_unlock(&g_voice.lock);
          continue;
        }

      count = ny_voice_capture_convert(resample, g_voice_mic,
                                       NY_VOICE_MIC_FRAMES, g_voice_chunk);
      level = ny_voice_rms_s16(g_voice_chunk, count);
      ny_voice_gate_feed(&gate, level, NY_VOICE_CHUNK_MS);
      loud = gate.loud;

      nxmutex_lock(&g_voice.ring_lock);
      ny_voice_ring_write(&g_voice.ring, g_voice_chunk, count);
      if (loud)
        {
          g_voice.last_loud = g_voice.ring.total;
        }

      g_voice.gate_open = gate.open;
      g_voice.level = level;
      nxmutex_unlock(&g_voice.ring_lock);
      sem_post(&g_voice.captured);
    }

  ny_voice_audio_close(audio);
  nxmutex_lock(&g_voice.lock);
  g_voice.capture_open = false;
  nxmutex_unlock(&g_voice.lock);
  return NULL;
}

/****************************************************************************
 * ny_voice_pump
 ****************************************************************************/

static int ny_voice_pump_begin(struct ny_voice_port_s *port,
                               struct nyamp_buffer_s *grant)
{
  char labels[96];
  int ret;

  ret = ny_voice_port_open(port);
  if (ret < 0)
    {
      return ret;
    }

  if ((ny_compute_capabilities() & NY_COMPUTE_CAP_KWS) == 0)
    {
      ny_voice_model_set(&g_voice.kws, NY_VOICE_MODEL_ERROR, -ENOTSUP);
      return -ENOTSUP;
    }

  ret = ny_voice_model_load(port, &g_voice.kws, NYAMP_SERVICE_KWS, "kws",
                            NY_VOICE_LOAD_MS);
  if (ret == -EBUSY)
    {
      /* Loaded with other parameters (the sensitivity changed, or another
       * life of this task loaded it): drop it and load ours.
       */

      ny_voice_wire_kws_end(&port->wire);
      ny_voice_wire_unload(&port->wire, NYAMP_SERVICE_KWS);
      ret = ny_voice_model_load(port, &g_voice.kws, NYAMP_SERVICE_KWS, "kws",
                                NY_VOICE_LOAD_MS);
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = ny_voice_wire_kws_begin(&port->wire, NY_VOICE_WIRE_S16,
                                NY_VOICE_WINDOW_SAMPLES, grant);
  if (ret == -EBUSY)
    {
      /* A stream this task's previous life left behind still owns the
       * slot.  END ends it whatever id it carries; BEGIN is accepted once
       * its FINISH is out.
       */

      ny_voice_wire_kws_end(&port->wire);
      usleep(300000);
      ret = ny_voice_wire_kws_begin(&port->wire, NY_VOICE_WIRE_S16,
                                    NY_VOICE_WINDOW_SAMPLES, grant);
    }

  if (ret < 0)
    {
      ny_voice_error(ret, "wake word stream did not start");
      return ret;
    }

  if (ny_voice_wire_kws_labels(&port->wire, labels, sizeof(labels)) > 0)
    {
      syslog(LOG_INFO, "nyvoice: listening for %s\n", labels);
    }

  return 0;
}

static void ny_voice_pump_events(struct ny_voice_port_s *port, int timeout_ms)
{
  struct ny_voice_wire_frame_s frame;
  struct ny_voice_wire_event_s event;

  while (port->wire.request_id != 0 &&
         ny_voice_wire_event(&port->wire, &frame, &event, timeout_ms) == 1)
    {
      timeout_ms = 0;
      if (event.kind == NY_VOICE_WIRE_FINISH)
        {
          /* Nobody here ended it: the service did (UNLOAD by a diagnostic,
           * a backend error).  The loop starts a new stream.
           */

          nxmutex_lock(&g_voice.lock);
          g_voice.streaming = false;
          nxmutex_unlock(&g_voice.lock);
          return;
        }

      if (event.kind != NY_VOICE_WIRE_DETECTED)
        {
          continue;
        }

      nxmutex_lock(&g_voice.lock);
      g_voice.detections++;
      if (g_voice.state == NY_VOICE_SPEAKING && g_voice.speaking_wake)
        {
          /* The robot said the wake word itself. */

          nxmutex_unlock(&g_voice.lock);
          continue;
        }

      g_voice.wake = event.detected;
      g_voice.wake.label = NULL;
      snprintf(g_voice.wake_word, sizeof(g_voice.wake_word), "%.*s",
               (int)event.detected.label_length, event.detected.label);
      nxmutex_unlock(&g_voice.lock);
      ny_voice_post(NY_VOICE_EV_WAKE, true, false, 0);
    }
}

static void *ny_voice_pump_thread(void *arg)
{
  static struct ny_voice_port_s port;
  struct nyamp_buffer_s grant;
  uint64_t cursor = 0;
  uint64_t retry_at = 0;
  uint32_t sequence = 0;
  bool paused = true;
  bool restarted = false;

  (void)arg;
  memset(&grant, 0, sizeof(grant));
  for (;;)
    {
      enum ny_voice_state_e state;
      struct timespec until;
      uint64_t first;
      uint64_t next = 0;
      uint64_t total;
      uint64_t resumed;
      size_t count;
      bool streaming;
      bool reload;
      bool gap = false;
      bool gate;
      bool fresh;
      int ret;

      nxmutex_lock(&g_voice.lock);
      state = g_voice.state;
      streaming = g_voice.streaming;
      reload = g_voice.kws_reload;
      g_voice.kws_reload = false;
      if (g_voice.stopping)
        {
          nxmutex_unlock(&g_voice.lock);
          break;
        }

      nxmutex_unlock(&g_voice.lock);

      if (streaming && (state == NY_VOICE_OFF || reload))
        {
          /* Give the capture slot back; a changed sensitivity is another
           * model as far as the service is concerned.
           */

          ny_voice_wire_kws_end(&port.wire);
          ny_voice_pump_events(&port, 1000);
          port.wire.request_id = 0;
          if (reload)
            {
              ny_voice_wire_unload(&port.wire, NYAMP_SERVICE_KWS);
              ny_voice_model_set(&g_voice.kws, NY_VOICE_MODEL_UNLOADED, 0);
            }

          nxmutex_lock(&g_voice.lock);
          g_voice.streaming = false;
          nxmutex_unlock(&g_voice.lock);
          streaming = false;
        }

      if (state == NY_VOICE_OFF)
        {
          usleep(100000);
          continue;
        }

      if (!streaming)
        {
          if (ny_voice_now_ms() < retry_at || ny_compute_generation() == 0)
            {
              usleep(200000);
              continue;
            }

          ret = ny_voice_pump_begin(&port, &grant);
          if (ret < 0)
            {
              if (ret == -ENOTCONN || ret == -ECONNRESET)
                {
                  ny_voice_port_close(&port);
                }

              /* A model that is not on the device, or a daemon without the
               * backend, does not change by itself: ask rarely.
               */

              retry_at = ny_voice_now_ms() + (ret == -ENOENT || ret == -ENOTSUP
                                                  ? 6 * NY_VOICE_RETRY_MS
                                                  : NY_VOICE_RETRY_MS);
              continue;
            }

          /* A new stream has an empty ring on the compute domain: whatever
           * position the first window carries is where an ASR request may
           * attach from.
           */

          sequence = 0;
          paused = true;
          restarted = true;
          nxmutex_lock(&g_voice.lock);
          g_voice.streaming = true;
          nxmutex_unlock(&g_voice.lock);
        }

      ny_voice_pump_events(&port, 0);

      nxmutex_lock(&g_voice.lock);
      state = g_voice.state;
      streaming = g_voice.streaming && g_voice.capture_open &&
                  (state == NY_VOICE_IDLE || state == NY_VOICE_LISTENING ||
                   state == NY_VOICE_THINKING ||
                   (state == NY_VOICE_SPEAKING && NY_VOICE_DUPLEX &&
                    NY_VOICE_BARGE_IN));
      nxmutex_unlock(&g_voice.lock);

      nxmutex_lock(&g_voice.ring_lock);
      total = g_voice.ring.total;
      resumed = g_voice.resumed_at;
      gate = g_voice.gate_open;
      nxmutex_unlock(&g_voice.ring_lock);

#ifndef CONFIG_NYABULA_CORE_VOICE_VAD_GATE
      gate = true;
#endif

      /* The gate never holds back a command: once the recogniser listens
       * it needs the silence after the words to find their end.
       */

      if (state == NY_VOICE_LISTENING)
        {
          gate = true;
        }

      if (!streaming || (!gate && cursor + NY_VOICE_WINDOW_SAMPLES > total))
        {
          paused = true;
          ny_voice_pump_events(&port, 50);
          continue;
        }

      fresh = false;
      if (paused)
        {
          /* Resume a little in the past: the gate opens on the first loud
           * chunk, which may be the middle of the wake word.  Never before
           * the microphone was last opened, though -- what the ring holds
           * from before that is from before the robot spoke.
           */

          first = total > NY_VOICE_PREROLL_SAMPLES
                      ? total - NY_VOICE_PREROLL_SAMPLES
                      : 0;
          if (first < resumed)
            {
              first = resumed;
            }

          if (first > cursor)
            {
              cursor = first;
              fresh = true;
            }

          paused = false;
        }

      if (total < cursor + NY_VOICE_WINDOW_SAMPLES)
        {
          clock_gettime(CLOCK_REALTIME, &until);
          until.tv_nsec += 50000000;
          if (until.tv_nsec >= 1000000000)
            {
              until.tv_sec++;
              until.tv_nsec -= 1000000000;
            }

          sem_timedwait(&g_voice.captured, &until);
          continue;
        }

      nxmutex_lock(&g_voice.ring_lock);
      first = cursor;
      count = ny_voice_ring_read(&g_voice.ring, &first, g_voice_window,
                                 NY_VOICE_WINDOW_SAMPLES, &gap);
      fresh = fresh || gap || restarted || g_voice.capture_gap;
      g_voice.capture_gap = false;
      restarted = false;
      nxmutex_unlock(&g_voice.ring_lock);

      /* `first` is now one past the window; a reader that fell out of the
       * ring was moved, so the window's own position is derived from it.
       */

      cursor = first - count;
      ret = ny_voice_wire_kws_push(&port.wire, &grant, g_voice_window, count,
                                   sequence, fresh, cursor, &next);
      if (ret < 0)
        {
          /* Whatever it was, the stream is not trusted any more: a new
           * generation, a daemon that lost the model, a refused window.
           */

          ny_voice_error(ret, "wake word stream lost");
          port.wire.request_id = 0;
          if (ret == -ENOTCONN || ret == -ECONNRESET)
            {
              ny_voice_port_close(&port);
            }

          nxmutex_lock(&g_voice.lock);
          g_voice.streaming = false;
          nxmutex_unlock(&g_voice.lock);
          ny_voice_model_set(&g_voice.kws, NY_VOICE_MODEL_UNLOADED, ret);
          ny_voice_post(NY_VOICE_EV_LINK_LOST, false, false, 0);
          retry_at = ny_voice_now_ms() + 1000;
          continue;
        }

      sequence++;
      nxmutex_lock(&g_voice.lock);
      if (fresh)
        {
          g_voice.stream_base = cursor;
        }

      cursor += count;
      g_voice.stream_next = cursor;
      g_voice.windows++;
      nxmutex_unlock(&g_voice.lock);
    }

  if (port.wire.request_id != 0)
    {
      ny_voice_wire_kws_end(&port.wire);
    }

  ny_voice_port_close(&port);
  nxmutex_lock(&g_voice.lock);
  g_voice.streaming = false;
  nxmutex_unlock(&g_voice.lock);
  return NULL;
}

/****************************************************************************
 * ny_voice_play
 ****************************************************************************/

static bool ny_voice_play_cancelled(void)
{
  bool cancelled;

  nxmutex_lock(&g_voice.lock);
  cancelled = g_voice.stopping ||
              (int32_t)(g_voice.play_dropped - g_voice.play_current) >= 0;
  nxmutex_unlock(&g_voice.lock);
  return cancelled;
}

/****************************************************************************
 * Name: ny_voice_play_acquire
 *
 * Description:
 *   Get the speaker.  Bluetooth music lets go the way it does for the alarm
 *   chime (a phone call says no); music from flash is stopped, because the
 *   player keeps the node reserved even while paused; and where the codec
 *   cannot do both, the microphone is closed first.
 *
 ****************************************************************************/

static int ny_voice_play_acquire(void)
{
  const struct ny_product_caller_s caller = { NY_VOICE_CALLER,
                                              NY_PRODUCT_OWNER, true };

  cJSON *data = cJSON_CreateObject();
  cJSON *result = NULL;
  uint64_t deadline;
  int ret = 0;

#ifdef CONFIG_NYABULA_CORE_BT
  ret = ny_product_bt_speaker_claim(true);
  if (ret < 0)
    {
      cJSON_Delete(data);
      return ret;
    }
#endif

  if (data != NULL &&
      ny_product_request(&caller, "music.status", data, &result) == 0)
    {
      const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "state");

      if (cJSON_IsString(state) && strcmp(state->valuestring, "idle") != 0)
        {
          cJSON_Delete(result);
          result = NULL;
          ny_product_request(&caller, "music.stop", data, &result);
        }
    }

  cJSON_Delete(result);
  cJSON_Delete(data);

  if (!NY_VOICE_DUPLEX)
    {
      nxmutex_lock(&g_voice.lock);
      g_voice.capture_hold = true;
      nxmutex_unlock(&g_voice.lock);

      deadline = ny_voice_now_ms() + NY_VOICE_CLAIM_MS;
      for (;;)
        {
          bool open;

          nxmutex_lock(&g_voice.lock);
          open = g_voice.capture_open;
          nxmutex_unlock(&g_voice.lock);
          if (!open)
            {
              break;
            }

          if (ny_voice_now_ms() >= deadline)
            {
              ret = -ETIMEDOUT;
              break;
            }

          usleep(10000);
        }
    }

  return ret;
}

static void ny_voice_play_release(void)
{
  nxmutex_lock(&g_voice.lock);
  g_voice.capture_hold = false;
  nxmutex_unlock(&g_voice.lock);
#ifdef CONFIG_NYABULA_CORE_BT
  ny_product_bt_speaker_release();
#endif
}

/****************************************************************************
 * Name: ny_voice_play_convert / ny_voice_play_write
 *
 * Description:
 *   Mono float32 at the TTS rate into the output buffer, in whatever format
 *   the speaker was opened in; then the buffer to the codec.  `samples` may
 *   point into the shared arena: it is read once, front to back, so that
 *   the window can be released before it is heard.  The write blocks while
 *   every chunk is queued, which paces the player to the codec clock; a
 *   cancel is noticed between chunks.
 *
 ****************************************************************************/

static size_t ny_voice_play_convert(struct ny_voice_resample_s *resample,
                                    const float *samples, size_t count)
{
#ifdef CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_16K
  count = ny_voice_resample(resample, samples, count, g_voice_float,
                            NY_VOICE_TTS_WINDOW);
  ny_voice_f32_to_s16(g_voice_out, g_voice_float, count);
  return count * sizeof(int16_t);
#else
  (void)resample;
  ny_voice_f32_to_s16_stereo(g_voice_out, samples, count);
  return count * 2 * sizeof(int16_t);
#endif
}

static int ny_voice_play_write(struct ny_voice_audio_s *audio, size_t bytes)
{
  size_t done = 0;

  while (done < bytes)
    {
      size_t part = bytes - done < NY_VOICE_OUT_CHUNK ? bytes - done
                                                      : NY_VOICE_OUT_CHUNK;
      ssize_t written;

      if (ny_voice_play_cancelled())
        {
          return -ECANCELED;
        }

      written = ny_voice_audio_write(
          audio, (const uint8_t *)g_voice_out + done, part, 1000);
      if (written < 0)
        {
          return (int)written;
        }

      if (written == 0)
        {
          return -ETIMEDOUT; /* The codec stopped taking samples. */
        }

      done += (size_t)written;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_voice_play_speech
 *
 * Description:
 *   Text to the speaker, one sentence per TTS request.  The compute domain
 *   splits sentences too; doing it here as well keeps a request short, so
 *   that a cancel between two of them costs nothing and the first window
 *   of a long reply does not wait for the whole reply to be transferred.
 *
 *   A window is copied out of the shared slot and released at once: the
 *   service may then synthesize the next one while this one plays.  After a
 *   cancel or a new compute generation the outstanding window is void and
 *   is not read.
 *
 ****************************************************************************/

static int ny_voice_play_speech(struct ny_voice_port_s *port,
                                struct ny_voice_audio_s *audio,
                                struct ny_voice_resample_s *resample,
                                const char *text)
{
  static const int16_t silence[NY_VOICE_OUT_RATE / 20 * 2];
  char sentence[NY_VOICE_SENTENCE_MAX];
  size_t position = 0;
  uint32_t voice;
  float speed;
  int attempt;
  int ret;

  ret = ny_voice_port_open(port);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_voice.lock);
  voice = g_voice.settings.reply_voice;
  speed = (float)g_voice.settings.reply_speed / 100.0f;
  nxmutex_unlock(&g_voice.lock);

  while (ny_voice_sentence_next(text, &position, sentence, sizeof(sentence)) !=
         0)
    {
      struct ny_voice_wire_frame_s frame;
      struct ny_voice_wire_event_s event;

      for (attempt = 0;; attempt++)
        {
          ret = ny_voice_wire_tts_say(&port->wire, sentence, strlen(sentence),
                                      voice, speed, NY_VOICE_TTS_WINDOW);
          if (ret == -ENOENT && attempt == 0)
            {
              /* Not loaded: the first reply since boot, or the daemon
               * restarted.  Text the front end has nothing to say for
               * finishes with no window, so -ENOENT is never the text.
               */

              ret = ny_voice_model_load(port, &g_voice.tts, NYAMP_SERVICE_TTS,
                                        "tts", NY_VOICE_TTS_LOAD_MS);
              if (ret == 0)
                {
                  continue;
                }
            }
          else if (ret == -EBUSY && attempt < 10 && !ny_voice_play_cancelled())
            {
              /* A model pull owns the shared slot, or the request just
               * cancelled is still winding down.
               */

              usleep(300000);
              continue;
            }

          break;
        }

      if (ret < 0)
        {
          return ret;
        }

      ny_voice_model_set(&g_voice.tts, NY_VOICE_MODEL_READY, 0);
      for (;;)
        {
          uint8_t *window;
          size_t bytes;

          if (ny_voice_play_cancelled())
            {
              ny_voice_wire_cancel(&port->wire, NYAMP_SERVICE_TTS);
              port->wire.request_id = 0;
              return -ECANCELED;
            }

          ret = ny_voice_wire_event(&port->wire, &frame, &event, 50);
          if (ret < 0)
            {
              port->wire.request_id = 0;
              return ret;
            }

          if (ret == 0)
            {
              /* Synthesis is slower than playback right now.  Silence keeps
               * the codec fed: a stream that runs dry would have to be
               * started over, WAV header and all.
               */

              if (ny_voice_audio_queued(audio) <
                  NY_VOICE_OUT_RATE / 10 * NY_VOICE_OUT_CHANNELS * 2)
                {
                  ny_voice_audio_write(
                      audio, silence,
                      NY_VOICE_OUT_RATE / 20 * NY_VOICE_OUT_CHANNELS * 2, 0);
                }

              continue;
            }

          if (event.kind == NY_VOICE_WIRE_FINISH)
            {
              if (event.status != NYAMP_MODEL_OK &&
                  event.status != NYAMP_MODEL_UNSUPPORTED)
                {
                  return ny_voice_wire_errno(event.status);
                }

              /* UNSUPPORTED: a piece the vocoder cannot take whole.  What
               * could be said has been; go on with the next sentence.
               */

              break;
            }

          if (event.kind != NY_VOICE_WIRE_PCM)
            {
              continue;
            }

          if (event.sample_rate != NY_VOICE_TTS_RATE || event.channels != 1 ||
              event.buffer.format != NYAMP_FORMAT_F32 ||
              event.valid_samples > NY_VOICE_TTS_WINDOW ||
              event.buffer.length != event.valid_samples * 4 ||
              ny_voice_wire_window(&port->wire, &event.buffer,
                                   event.buffer.length, &window) < 0)
            {
              ny_voice_wire_cancel(&port->wire, NYAMP_SERVICE_TTS);
              port->wire.request_id = 0;
              return -EPROTO;
            }

          /* Convert (that is the copy out of the slot), release, play:
           * the service synthesizes the next window meanwhile.
           */

          bytes = ny_voice_play_convert(
              resample, (const float *)(void *)window, event.valid_samples);
          ny_voice_wire_tts_release(&port->wire, &event);
          ret = ny_voice_play_write(audio, bytes);
          if (ret < 0)
            {
              ny_voice_wire_cancel(&port->wire, NYAMP_SERVICE_TTS);
              port->wire.request_id = 0;
              return ret;
            }
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_voice_play_cue
 *
 * Description:
 *   A cue without the TTS model: CUE_DIR/<name>.wav (wait, not_heard, busy,
 *   error, approval) when the owner put one there -- `nyampctl tts say
 *   <text> <file>` makes them -- else two short tones.
 *
 ****************************************************************************/

static int ny_voice_play_cue(struct ny_voice_audio_s *audio,
                             struct ny_voice_resample_s *resample,
                             const char *name)
{
  static float tone[NY_VOICE_TTS_RATE / 5];
  char path[96];
  uint8_t header[256];
  uint32_t rate = 0;
  uint32_t bytes = 0;
  uint16_t channels = 0;
  size_t offset = 0;
  size_t index;
  ssize_t got;
  int ret = 0;
  int fd;

  snprintf(path, sizeof(path), "%s/%s.wav", CONFIG_NYABULA_CORE_VOICE_CUE_DIR,
           name);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  got = fd < 0 ? -1 : read(fd, header, sizeof(header));
  if (got > 0 &&
      ny_voice_wav_parse(header, (size_t)got, &rate, &channels, &offset,
                         &bytes) == 0 &&
      rate == NY_VOICE_TTS_RATE && channels == 1 &&
      lseek(fd, (off_t)offset, SEEK_SET) >= 0)
    {
      static int16_t pcm[2048];

      while (ret == 0 && (got = read(fd, pcm, sizeof(pcm))) > 0)
        {
          size_t count = (size_t)got / sizeof(int16_t);

          ny_voice_s16_to_f32(tone, pcm, count);
          ret = ny_voice_play_write(
              audio, ny_voice_play_convert(resample, tone, count));
        }

      close(fd);
      return ret;
    }

  if (fd >= 0)
    {
      close(fd);
    }

  /* 80 ms of 660 Hz, 40 ms of nothing, 80 ms of 880 Hz, with 5 ms ramps so
   * the speaker does not click.
   */

  memset(tone, 0, sizeof(tone));
  for (index = 0; index < NY_VOICE_TTS_RATE / 5; index++)
    {
      size_t in_tone = index % (NY_VOICE_TTS_RATE * 3 / 25);
      size_t length = NY_VOICE_TTS_RATE * 2 / 25;
      size_t ramp = NY_VOICE_TTS_RATE / 200;
      double hz = index < NY_VOICE_TTS_RATE * 3 / 25 ? 660.0 : 880.0;
      double gain;

      if (in_tone >= length)
        {
          continue;
        }

      gain = in_tone < ramp ? (double)in_tone / (double)ramp
             : in_tone > length - ramp
                 ? (double)(length - in_tone) / (double)ramp
                 : 1.0;
      tone[index] = (float)(0.25 * gain *
                            sin(2.0 * 3.14159265358979 * hz * (double)in_tone /
                                (double)NY_VOICE_TTS_RATE));
    }

  return ny_voice_play_write(
      audio, ny_voice_play_convert(resample, tone, NY_VOICE_TTS_RATE / 5));
}

static void *ny_voice_play_thread(void *arg)
{
  static struct ny_voice_port_s port;
  static char text[NY_VOICE_REPLY_MAX + 1];
  struct ny_voice_resample_s *resample = NULL;
  uint32_t served = 0;

  (void)arg;
  port.player = true;
#ifdef CONFIG_NYABULA_CORE_VOICE_CODEC_SHARED_16K
  resample = &g_voice_resample;
#endif

  for (;;)
    {
      struct ny_voice_audio_config_s config;
      struct ny_voice_audio_s *audio = NULL;
      enum ny_voice_cue_e cue;
      const char *name;
      uint32_t job;
      bool ready;
      int ret;

      while (sem_wait(&g_voice.play_wake) < 0)
        {
        }

      nxmutex_lock(&g_voice.lock);
      if (g_voice.stopping)
        {
          nxmutex_unlock(&g_voice.lock);
          break;
        }

      /* Only the newest job is played: one that was replaced while it
       * waited here has been cancelled by definition.  The text is copied
       * because the next job may be written while this one still plays.
       */

      job = g_voice.play_job;
      if (job == served)
        {
          nxmutex_unlock(&g_voice.lock);
          continue;
        }

      served = job;
      g_voice.play_current = job;
      cue = g_voice.play_cue;
      name = g_voice.play_name;
      ready = g_voice.tts.state == NY_VOICE_MODEL_READY;
      strlcpy(text, g_voice.play_text, sizeof(text));
      nxmutex_unlock(&g_voice.lock);

      ret = ny_voice_play_acquire();
      if (ret == 0)
        {
          memset(&config, 0, sizeof(config));
          config.path = CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE;
          config.wav = true;
          config.rate = NY_VOICE_OUT_RATE;
          config.channels = NY_VOICE_OUT_CHANNELS;
          config.chunk = NY_VOICE_OUT_CHUNK;
          ret = ny_voice_audio_open(&audio, &config);
        }

      if (ret == 0)
        {
          if (resample != NULL)
            {
              ny_voice_resample_init(resample);
            }

          /* A short cue is not worth the ten seconds a first TTS load
           * takes; once the model is there it says the cue properly.
           */

          if (cue == NY_VOICE_CUE_SHORT && !ready)
            {
              ret = ny_voice_play_cue(audio, resample, name);
            }
          else
            {
              ret = ny_voice_play_speech(&port, audio, resample, text);
              if (ret < 0 && ret != -ECANCELED && cue != NY_VOICE_CUE_NONE)
                {
                  ret = ny_voice_play_cue(audio, resample, name);
                }
            }

          if (ret == 0)
            {
              ny_voice_audio_drain(audio, 3000);
            }
        }

      if (ret < 0 && ret != -ECANCELED)
        {
          ny_voice_error(ret, ret == -EBUSY ? "the speaker is taken"
                                            : "speech failed");
          if (ret == -ENOTCONN || ret == -ECONNRESET)
            {
              ny_voice_port_close(&port);
              ny_voice_model_set(&g_voice.tts, NY_VOICE_MODEL_UNLOADED, ret);
            }
        }

      ny_voice_audio_close(audio);
      ny_voice_play_release();

      nxmutex_lock(&g_voice.lock);
      if (g_voice.play_job == job)
        {
          g_voice.play_busy = false;
        }

      nxmutex_unlock(&g_voice.lock);
      ny_voice_post(NY_VOICE_EV_SPEAK_DONE, ret == 0, false, job);
    }

  ny_voice_port_close(&port);
  return NULL;
}

/****************************************************************************
 * ny_voice_eyes
 ****************************************************************************/

static void *ny_voice_eyes_thread(void *arg)
{
  static const char *const names[] = { "idle", "curious", "processing",
                                       "happy" };

  const struct ny_product_caller_s caller = { NY_VOICE_CALLER,
                                              NY_PRODUCT_OWNER, true };

  const char *shown = NULL;
  int held = -1;

  (void)arg;
  for (;;)
    {
      cJSON *data;
      cJSON *result = NULL;
      struct timespec until;
      bool renew = false;
      int wanted;

      /* An expression is a lease of NYABULA_EYE_DEFAULT_LEASE_MS.  Set once
       * on entering a state, the "processing" face lapsed after five seconds
       * while the model was still thinking, and the eyes went idle before
       * the robot spoke (seen on the board).  The face this service put on
       * is therefore renewed at less than half the lease until the state
       * machine asks for another one.
       */

      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += NY_VOICE_EYES_RENEW_S;
      if (sem_timedwait(&g_voice.eyes_wake, &until) < 0)
        {
          if (errno != ETIMEDOUT)
            {
              continue;
            }

          renew = true;
        }

      nxmutex_lock(&g_voice.lock);
      wanted = g_voice.eyes_wanted;
      g_voice.eyes_wanted = -1;
      if (wanted < 0 && renew && held > NY_VOICE_EYES_RESTORE)
        {
          wanted = held;
        }

      if (g_voice.stopping)
        {
          nxmutex_unlock(&g_voice.lock);
          break;
        }

      nxmutex_unlock(&g_voice.lock);
      if (wanted < 0 || wanted > NY_VOICE_EYES_SPEAKING)
        {
          continue; /* Overtaken by a later request. */
        }

      data = cJSON_CreateObject();
      if (data == NULL)
        {
          continue;
        }

      if (wanted == NY_VOICE_EYES_RESTORE)
        {
          const cJSON *now = NULL;

          /* Put the face back only if it still is the one this service
           * put on: the agent may have been asked for an expression during
           * the very turn that is ending.
           */

          if (shown != NULL &&
              ny_product_request(&caller, "eyes.status", data, &result) == 0)
            {
              now = cJSON_GetObjectItemCaseSensitive(result, "expression");
            }

          if (!cJSON_IsString(now) || strcmp(now->valuestring, shown) != 0)
            {
              shown = NULL;
              held = -1;
              cJSON_Delete(result);
              cJSON_Delete(data);
              continue;
            }

          cJSON_Delete(result);
          result = NULL;
        }

      if (cJSON_AddStringToObject(data, "expression", names[wanted]) != NULL &&
          ny_product_request(&caller, "eyes.expression", data, &result) == 0)
        {
          shown = wanted == NY_VOICE_EYES_RESTORE ? NULL : names[wanted];
          held = wanted == NY_VOICE_EYES_RESTORE ? -1 : wanted;
        }

      cJSON_Delete(result);
      cJSON_Delete(data);
    }

  return NULL;
}

/****************************************************************************
 * ny_voice_sm: the turn
 ****************************************************************************/

/****************************************************************************
 * Name: ny_voice_topic
 *
 * Description:
 *   One request to another product service, as the owner.
 *
 ****************************************************************************/

static int ny_voice_topic(const char *topic, const char *json, cJSON **result)
{
  const struct ny_product_caller_s caller = { NY_VOICE_CALLER,
                                              NY_PRODUCT_OWNER, true };

  cJSON *data = cJSON_Parse(json);
  int ret;

  *result = NULL;
  if (data == NULL)
    {
      return -ENOMEM;
    }

  ret = ny_product_request(&caller, topic, data, result);
  cJSON_Delete(data);
  return ret;
}

/****************************************************************************
 * Name: ny_voice_turn_say / ny_voice_turn_speak
 *
 * Description:
 *   Give the player its next job.  `text` may be one of the texts this
 *   service keeps (the reply, voice.say's): it is copied under the lock
 *   that guards them.
 *
 ****************************************************************************/

static void ny_voice_turn_say(const char *text, enum ny_voice_cue_e cue,
                              const char *name)
{
  const char *at;

  nxmutex_lock(&g_voice.lock);
  strlcpy(g_voice.play_text, text, sizeof(g_voice.play_text));
  g_voice.play_cue = cue;
  g_voice.play_name = name;
  g_voice.play_busy = true;
  g_voice.play_job++;

  /* Would the robot wake itself by saying this? */

  g_voice.speaking_wake = false;
  for (at = g_voice.play_text; *at != '\0'; at++)
    {
      if (strncasecmp(at, "openvela", 8) == 0)
        {
          g_voice.speaking_wake = true;
          break;
        }
    }

  nxmutex_unlock(&g_voice.lock);
  sem_post(&g_voice.play_wake);
}

static void ny_voice_turn_speak(enum ny_voice_speech_e speech)
{
  switch (speech)
    {
      case NY_VOICE_SPEECH_REPLY:
        ny_voice_turn_say(g_voice.reply, NY_VOICE_CUE_NONE, "reply");
        break;

      case NY_VOICE_SPEECH_TEXT:
        ny_voice_turn_say(g_voice.say_text, NY_VOICE_CUE_NONE, "say");
        break;

      case NY_VOICE_SPEECH_APPROVAL:
        ny_voice_turn_say(NY_VOICE_CUE_APPROVAL, NY_VOICE_CUE_FIXED,
                          "approval");
        break;

      case NY_VOICE_SPEECH_NOT_HEARD:
        ny_voice_turn_say(NY_VOICE_CUE_NOT_HEARD, NY_VOICE_CUE_SHORT,
                          "not_heard");
        break;

      case NY_VOICE_SPEECH_BUSY:
        ny_voice_turn_say(NY_VOICE_CUE_BUSY, NY_VOICE_CUE_SHORT, "busy");
        break;

      default:
        ny_voice_turn_say(NY_VOICE_CUE_ERROR, NY_VOICE_CUE_SHORT, "error");
        break;
    }
}

/****************************************************************************
 * Name: ny_voice_turn_speak_cancel
 *
 * Description:
 *   Stop the player and wait until it has let go of the codec: what follows
 *   a barge-in is the microphone, and in the HALF arrangement that cannot
 *   open while the speaker is.
 *
 ****************************************************************************/

static void ny_voice_turn_speak_cancel(void)
{
  uint64_t deadline = ny_voice_now_ms() + 2000;

  nxmutex_lock(&g_voice.lock);
  g_voice.play_dropped = g_voice.play_job;
  nxmutex_unlock(&g_voice.lock);
  for (;;)
    {
      bool busy;

      nxmutex_lock(&g_voice.lock);
      busy = g_voice.play_busy;
      nxmutex_unlock(&g_voice.lock);
      if (!busy || ny_voice_now_ms() >= deadline)
        {
          break;
        }

      usleep(10000);
    }
}

/****************************************************************************
 * Name: ny_voice_turn_asr_start
 *
 * Description:
 *   Attach a recognition request to the wake word stream: after the wake
 *   phrase when a wake word led here, at the stream head for push-to-talk.
 *   The model is loaded on the first use, which is announced, because the
 *   owner would otherwise talk into a robot that is not listening yet.
 *
 ****************************************************************************/

static void ny_voice_turn_asr_start(struct ny_voice_turn_s *turn, bool attach)
{
  struct ny_voice_attach_s where;
  uint64_t deadline;
  uint32_t max_samples;
  bool streaming;
  bool cued = false;
  int attempt;
  int ret;

  memset(&where, 0, sizeof(where));
  nxmutex_lock(&g_voice.lock);
  g_voice.transcript[0] = '\0';
  g_voice.transcript_ms = 0;
  streaming = g_voice.streaming;
  max_samples =
      (g_voice.settings.max_listen_ms + 2000) / 1000 * NY_VOICE_CAPTURE_RATE;
  nxmutex_unlock(&g_voice.lock);

  ret = streaming ? ny_voice_port_open(&turn->asr) : -ENOTCONN;
  if (ret == 0 && (ny_compute_capabilities() & NY_COMPUTE_CAP_ASR) == 0)
    {
      ny_voice_model_set(&g_voice.asr, NY_VOICE_MODEL_ERROR, -ENOTSUP);
      ret = -ENOTSUP;
    }

  for (attempt = 0; ret == 0; attempt++)
    {
      nxmutex_lock(&g_voice.lock);
      where.has_offsets =
          attach && !cued &&
          (g_voice.wake.flags & NYAMP_KWS_DETECTED_HAS_OFFSETS) != 0;
      where.start_sample = g_voice.wake.start_sample;
      where.end_sample = g_voice.wake.end_sample;
      where.trigger_sample =
          attach && !cued ? g_voice.wake.trigger_sample : g_voice.stream_next;
      where.stream_next = g_voice.stream_next;
      where.stream_base = g_voice.stream_base;
      nxmutex_unlock(&g_voice.lock);
      where.ring_samples = NYAMP_KWS_RING_SECONDS * NY_VOICE_CAPTURE_RATE;
      where.backoff_samples = attach && !cued ? NY_VOICE_CAPTURE_RATE / 2 : 0;
      where.margin_samples = NY_VOICE_CAPTURE_RATE / 2;
      turn->asr_start = ny_voice_attach_sample(&where, NULL, NULL);

      ret = ny_voice_wire_asr_attach(&turn->asr.wire, max_samples,
                                     turn->asr_start);
      if (ret != -ENOENT || attempt != 0)
        {
          break;
        }

      /* NOT_READY: no model (the first wake since boot, or a restarted
       * daemon) -- or no stream, which the pump is about to find out.
       */

      ny_voice_turn_say(NY_VOICE_CUE_WAIT, NY_VOICE_CUE_SHORT, "wait");
      cued = true;
      ret = ny_voice_model_load(&turn->asr, &g_voice.asr, NYAMP_SERVICE_ASR,
                                "asr", NY_VOICE_LOAD_MS);

      /* The cue is a job of the player like any other: it must be over,
       * and the microphone open again, before the command is heard.  What
       * was said meanwhile is lost in the HALF arrangement, which is why
       * the request then attaches at the stream head instead of after the
       * wake phrase.
       */

      deadline = ny_voice_now_ms() + 5000;
      for (;;)
        {
          bool busy;

          nxmutex_lock(&g_voice.lock);
          busy = g_voice.play_busy || !g_voice.capture_open;
          nxmutex_unlock(&g_voice.lock);
          if (!busy || ny_voice_now_ms() >= deadline)
            {
              break;
            }

          usleep(20000);
        }
    }

  if (ret == 0)
    {
      ny_voice_model_set(&g_voice.asr, NY_VOICE_MODEL_READY, 0);
      turn->asr_open = true;
      turn->asr_silent = false;
    }
  else
    {
      ny_voice_error(ret, !streaming ? "no wake word stream to attach to"
                                     : "recognition did not start");
      if (ret == -ENOTCONN || ret == -ECONNRESET)
        {
          ny_voice_port_close(&turn->asr);
        }
    }

  ny_voice_post(ret == 0 ? NY_VOICE_EV_ASR_STARTED : NY_VOICE_EV_ASR_FAILED,
                ret == 0, false, 0);
}

static void ny_voice_turn_asr_poll(struct ny_voice_turn_s *turn)
{
  struct ny_voice_wire_frame_s frame;
  struct ny_voice_wire_event_s event;
  int ret;

  while (turn->asr_open)
    {
      ret = ny_voice_wire_event(&turn->asr.wire, &frame, &event, 0);
      if (ret == 0)
        {
          return;
        }

      if (ret < 0)
        {
          /* The daemon is gone and the request with it. */

          turn->asr_open = false;
          turn->asr.wire.request_id = 0;
          if (ret == -ENOTCONN || ret == -ECONNRESET)
            {
              ny_voice_port_close(&turn->asr);
            }

          ny_voice_model_set(&g_voice.asr, NY_VOICE_MODEL_UNLOADED, ret);
          if (!turn->asr_silent)
            {
              ny_voice_post(NY_VOICE_EV_ASR_FINISHED, false, true, 0);
            }

          return;
        }

      if (event.kind == NY_VOICE_WIRE_PARTIAL)
        {
          size_t before;

          nxmutex_lock(&g_voice.lock);
          before = strlen(g_voice.transcript);
          ny_voice_wire_transcript(g_voice.transcript,
                                   sizeof(g_voice.transcript), &event);
          if (strlen(g_voice.transcript) != before ||
              (event.flags & NYAMP_ASR_PARTIAL_RESYNC) != 0)
            {
              g_voice.transcript_ms = ny_voice_now_ms();
            }

          nxmutex_unlock(&g_voice.lock);
          if ((event.flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0 &&
              !turn->asr_silent)
            {
              ny_voice_post(NY_VOICE_EV_ASR_ENDPOINT, true, false, 0);
            }
        }
      else if (event.kind == NY_VOICE_WIRE_FINISH)
        {
          bool empty;

          turn->asr_open = false;
          nxmutex_lock(&g_voice.lock);
          empty = g_voice.transcript[strspn(g_voice.transcript, " ")] == '\0';
          nxmutex_unlock(&g_voice.lock);
          if (!turn->asr_silent)
            {
              ny_voice_post(NY_VOICE_EV_ASR_FINISHED,
                            event.status == NYAMP_MODEL_OK, empty, 0);
            }
        }
    }
}

/****************************************************************************
 * Name: ny_voice_turn_submit
 *
 * Description:
 *   Hand the transcript to the agent through the same topic the panel uses
 *   (agent.chat), and follow the run with agent.run.get.  Turns that follow
 *   each other within five minutes share a conversation, so "and tomorrow?"
 *   works; after that a new one starts, which keeps the history the agent
 *   sends with every turn from growing for ever.
 *
 ****************************************************************************/

static void ny_voice_turn_submit(struct ny_voice_turn_s *turn)
{
  uint64_t now = ny_voice_now_ms();
  cJSON *result = NULL;
  cJSON *data;
  char request[48];
  bool valid;
  int ret;

  if (turn->conversation[0] == '\0' ||
      now - turn->last_turn_ms > NY_VOICE_SESSION_MS)
    {
      snprintf(turn->conversation, sizeof(turn->conversation),
               "voice-%" PRIu64, ny_product_time_ms(false) / 1000);
    }

  snprintf(request, sizeof(request), "voice-%" PRIu64 "-%" PRIu32,
           ny_product_time_ms(false), ++turn->request);

  data = cJSON_CreateObject();
  nxmutex_lock(&g_voice.lock);
  valid = data != NULL &&
          cJSON_AddStringToObject(data, "requestId", request) != NULL &&
          cJSON_AddStringToObject(data, "conversationId",
                                  turn->conversation) != NULL &&
          cJSON_AddStringToObject(data, "text", g_voice.transcript) != NULL;
  g_voice.reply[0] = '\0';
  g_voice.turns++;
  nxmutex_unlock(&g_voice.lock);

  if (valid)
    {
      const struct ny_product_caller_s caller = { NY_VOICE_CALLER,
                                                  NY_PRODUCT_OWNER, true };

      ret = ny_product_request(&caller, "agent.chat", data, &result);
    }
  else
    {
      ret = -ENOMEM;
    }

  cJSON_Delete(data);
  turn->submit_due = false;
  if (ret == 0)
    {
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "id");

      strlcpy(turn->run, cJSON_IsString(id) ? id->valuestring : "",
              sizeof(turn->run));
      turn->run_pending = false;
      turn->poll_at = 0;
      turn->last_turn_ms = now;
      if (turn->run[0] == '\0')
        {
          ret = -EPROTO;
        }
    }
  else if ((ret == -EBUSY || ret == -EAGAIN) && now < turn->submit_until)
    {
      /* The run this turn interrupted is still winding down, or the agent
       * is still starting.  The state machine keeps THINKING meanwhile.
       */

      turn->submit_due = true;
      turn->poll_at = now + 500;
    }

  cJSON_Delete(result);
  if (ret < 0 && !turn->submit_due)
    {
      ny_voice_error(ret, ret == -ENODATA ? "the agent has no model configured"
                          : ret == -EBUSY ? "the agent is busy"
                                          : "the agent refused the turn");
      ny_voice_post(ret == -EBUSY ? NY_VOICE_EV_AGENT_BUSY
                                  : NY_VOICE_EV_AGENT_FAILED,
                    false, false, 0);
    }
}

static void ny_voice_turn_agent_poll(struct ny_voice_turn_s *turn)
{
  uint64_t now = ny_voice_now_ms();
  cJSON *result = NULL;
  const cJSON *state;
  const cJSON *reply;
  char json[96];

  if (now < turn->poll_at)
    {
      return;
    }

  if (turn->submit_due)
    {
      ny_voice_turn_submit(turn);
      return;
    }

  if (turn->run[0] == '\0')
    {
      return;
    }

  turn->poll_at = now + NY_VOICE_AGENT_POLL_MS;
  snprintf(json, sizeof(json), "{\"id\":\"%s\"}", turn->run);
  if (ny_voice_topic("agent.run.get", json, &result) < 0)
    {
      cJSON_Delete(result);
      return; /* The store was busy; the next poll asks again. */
    }

  state = cJSON_GetObjectItemCaseSensitive(result, "state");
  reply = cJSON_GetObjectItemCaseSensitive(result, "reply");
  if (cJSON_IsString(state) &&
      strcmp(state->valuestring, "waiting_approval") == 0)
    {
      if (!turn->run_pending)
        {
          turn->run_pending = true;
          ny_voice_post(NY_VOICE_EV_AGENT_PENDING, true, false, 0);
        }

      /* A pending run is looked at less often: nothing waits for it. */

      turn->poll_at = now + 1000;
    }
  else if (cJSON_IsString(state) &&
           (strcmp(state->valuestring, "succeeded") == 0 ||
            strcmp(state->valuestring, "failed") == 0 ||
            strcmp(state->valuestring, "cancelled") == 0 ||
            strcmp(state->valuestring, "unknown") == 0))
    {
      bool ok = strcmp(state->valuestring, "succeeded") == 0;
      bool empty;

      nxmutex_lock(&g_voice.lock);
      strlcpy(g_voice.reply, cJSON_IsString(reply) ? reply->valuestring : "",
              sizeof(g_voice.reply));
      empty = g_voice.reply[0] == '\0';
      nxmutex_unlock(&g_voice.lock);
      turn->run[0] = '\0';
      turn->run_pending = false;
      ny_voice_post(NY_VOICE_EV_AGENT_REPLY, ok, empty, 0);
    }

  cJSON_Delete(result);
}

static void ny_voice_turn_act(struct ny_voice_turn_s *turn,
                              const struct ny_voice_sm_result_s *result)
{
  unsigned int index;
  cJSON *ignored;
  char json[96];

  for (index = 0; index < result->count; index++)
    {
      const struct ny_voice_sm_action_s *action = &result->actions[index];

      switch (action->type)
        {
          case NY_VOICE_ACT_ASR_START:
            ny_voice_turn_asr_start(turn, action->attach);
            break;

          case NY_VOICE_ACT_ASR_END:
            {
              uint64_t end;
              int ret;

              nxmutex_lock(&g_voice.lock);
              end = g_voice.stream_next;
              nxmutex_unlock(&g_voice.lock);
              ret = turn->asr_open
                        ? ny_voice_wire_asr_end(&turn->asr.wire, end)
                        : -ENOENT;
              if (ret < 0)
                {
                  turn->asr_open = false;
                  turn->asr.wire.request_id = 0;
                  ny_voice_post(NY_VOICE_EV_ASR_FINISHED, false, true, 0);
                }
            }
            break;

          case NY_VOICE_ACT_ASR_CANCEL:
            if (turn->asr_open)
              {
                /* Its FINISH still comes, and is nobody's news. */

                turn->asr_silent = true;
                ny_voice_wire_cancel(&turn->asr.wire, NYAMP_SERVICE_ASR);
              }

            break;

          case NY_VOICE_ACT_AGENT_SUBMIT:
            turn->submit_until = ny_voice_now_ms() + NY_VOICE_AGENT_RETRY_MS;
            ny_voice_turn_submit(turn);
            break;

          case NY_VOICE_ACT_AGENT_CANCEL:
            if (turn->run[0] != '\0')
              {
                snprintf(json, sizeof(json), "{\"id\":\"%s\"}", turn->run);
                ny_voice_topic("agent.cancel", json, &ignored);
                cJSON_Delete(ignored);
              }

            turn->run[0] = '\0';
            turn->submit_due = false;
            break;

          case NY_VOICE_ACT_AGENT_FORGET:
            turn->run[0] = '\0';
            turn->run_pending = false;
            break;

          case NY_VOICE_ACT_SPEAK:
            ny_voice_turn_speak(action->speech);
            break;

          case NY_VOICE_ACT_SPEAK_CANCEL:
            ny_voice_turn_speak_cancel();
            break;

          case NY_VOICE_ACT_EYES:
            nxmutex_lock(&g_voice.lock);
            g_voice.eyes_wanted = (int)action->eyes;
            nxmutex_unlock(&g_voice.lock);
            sem_post(&g_voice.eyes_wake);
            break;

          default:
            break;
        }
    }
}

/****************************************************************************
 * Name: ny_voice_turn_idle
 *
 * Description:
 *   Housekeeping between turns: notice a compute domain that restarted
 *   (every model it held is gone), and give memory back on the compute
 *   domain when the owner has not spoken for a long time.  The LLM, the TTS
 *   model (about 200 MB resident) and the recogniser share one Linux; the
 *   wake word model is small and stays.
 *
 ****************************************************************************/

static void ny_voice_turn_idle(struct ny_voice_turn_s *turn)
{
  uint32_t generation = ny_compute_generation();
  uint64_t now = ny_voice_now_ms();

  if (generation != turn->generation)
    {
      if (turn->generation != 0)
        {
          ny_voice_model_set(&g_voice.asr, NY_VOICE_MODEL_UNLOADED, 0);
          ny_voice_model_set(&g_voice.tts, NY_VOICE_MODEL_UNLOADED, 0);
          ny_voice_post(NY_VOICE_EV_LINK_LOST, false, false, 0);
        }

      turn->generation = generation;
    }

  if (turn->sm.state != NY_VOICE_IDLE)
    {
      turn->idle_since = now;
      return;
    }

  if (CONFIG_NYABULA_CORE_VOICE_UNLOAD_IDLE_S > 0 && generation != 0 &&
      now - turn->idle_since >=
          (uint64_t)CONFIG_NYABULA_CORE_VOICE_UNLOAD_IDLE_S * 1000)
    {
      bool asr;
      bool tts;

      nxmutex_lock(&g_voice.lock);
      asr = g_voice.asr.state == NY_VOICE_MODEL_READY;
      tts = g_voice.tts.state == NY_VOICE_MODEL_READY;
      nxmutex_unlock(&g_voice.lock);
      turn->idle_since = now;

      /* Both UNLOADs go through the turn thread's port: a port is only a
       * route for the response, not a claim on a service.
       */

      if ((asr || tts) && ny_voice_port_open(&turn->asr) == 0)
        {
          if (tts &&
              ny_voice_wire_unload(&turn->asr.wire, NYAMP_SERVICE_TTS) == 0)
            {
              ny_voice_model_set(&g_voice.tts, NY_VOICE_MODEL_UNLOADED, 0);
            }

          if (asr &&
              ny_voice_wire_unload(&turn->asr.wire, NYAMP_SERVICE_ASR) == 0)
            {
              ny_voice_model_set(&g_voice.asr, NY_VOICE_MODEL_UNLOADED, 0);
            }
        }
    }
}

static int ny_voice_task(int argc, char **argv)
{
  static struct ny_voice_turn_s turn;
  struct ny_voice_sm_config_s config;
  struct sched_param param;
  pthread_attr_t attr;
  pthread_t capture;
  pthread_t pump;
  pthread_t play;
  pthread_t eyes;
  bool enabled;
  int started = 0;

  (void)argc;
  (void)argv;

  memset(&turn, 0, sizeof(turn));
  ny_voice_settings_load();
  ny_voice_sm_defaults(&config);
  nxmutex_lock(&g_voice.lock);
  config.max_listen_ms = g_voice.settings.max_listen_ms;
  enabled = g_voice.settings.enabled;
  nxmutex_unlock(&g_voice.lock);
  ny_voice_sm_init(&turn.sm, &config);
  if (!enabled)
    {
      /* Switched off: no threads, no stacks.  voice.enable starts the task
       * again.
       */

      nxmutex_lock(&g_voice.lock);
      g_voice.running = false;
      nxmutex_unlock(&g_voice.lock);
      return 0;
    }

  /* The microphone thread runs above the rest: a chunk it does not fetch
   * in time is a hole in the wake word stream.
   */

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  param.sched_priority = CONFIG_NYABULA_CORE_VOICE_PRIORITY + 10;
  pthread_attr_setschedparam(&attr, &param);
  if (pthread_create(&capture, &attr, ny_voice_capture_thread, NULL) == 0)
    {
      pthread_setname_np(capture, "ny_voice_capture");
      started |= 1;
    }

  param.sched_priority = CONFIG_NYABULA_CORE_VOICE_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  if (pthread_create(&pump, &attr, ny_voice_pump_thread, NULL) == 0)
    {
      pthread_setname_np(pump, "ny_voice_pump");
      started |= 2;
    }

  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&play, &attr, ny_voice_play_thread, NULL) == 0)
    {
      pthread_setname_np(play, "ny_voice_play");
      started |= 4;
    }

  param.sched_priority = CONFIG_NYABULA_CORE_VOICE_PRIORITY - 10;
  pthread_attr_setschedparam(&attr, &param);
  if (pthread_create(&eyes, &attr, ny_voice_eyes_thread, NULL) == 0)
    {
      pthread_setname_np(eyes, "ny_voice_eyes");
      started |= 8;
    }

  pthread_attr_destroy(&attr);
  if (started != 15)
    {
      ny_voice_error(-ENOMEM, "a voice thread did not start");
    }
  else
    {
      ny_voice_post(NY_VOICE_EV_ENABLE, true, false, 0);
    }

  while (!ny_voice_stopping() && started == 15)
    {
      struct ny_voice_sm_event_s event;
      struct ny_voice_sm_result_s result;
      struct ny_voice_queued_s queued;
      uint64_t total;
      uint64_t loud;
      uint64_t text_ms;
      bool stale;

      memset(&event, 0, sizeof(event));
      if (ny_voice_take(&queued, 50))
        {
          /* Playback that was already replaced reports in late. */

          nxmutex_lock(&g_voice.lock);
          stale = queued.type == NY_VOICE_EV_SPEAK_DONE &&
                  queued.job != g_voice.play_job;
          nxmutex_unlock(&g_voice.lock);
          if (stale)
            {
              continue;
            }

          event.type = queued.type;
          event.ok = queued.ok;
          event.empty = queued.empty;
        }
      else
        {
          event.type = NY_VOICE_EV_TICK;
        }

      nxmutex_lock(&g_voice.ring_lock);
      total = g_voice.ring.total;
      loud = g_voice.last_loud;
      nxmutex_unlock(&g_voice.ring_lock);

      /* Speech is what the recognizer turned into text, not what the
       * energy gate found loud: the tail of the wake phrase is loud and
       * the command of somebody across the room is not, so the gate ended
       * turns before the owner had said a word and would have cut quiet
       * ones short.  The gate only tells how long the room has been quiet
       * when the recognizer's own endpoint does not come.
       */

      event.now_ms = ny_voice_now_ms();
      nxmutex_lock(&g_voice.lock);
      event.heard_speech =
          g_voice.transcript[strspn(g_voice.transcript, " ")] != '\0';
      text_ms = g_voice.transcript_ms;
      nxmutex_unlock(&g_voice.lock);
      event.silence_ms = 0;
      if (event.heard_speech && text_ms != 0 && event.now_ms > text_ms)
        {
          event.silence_ms = (uint32_t)(event.now_ms - text_ms);
          if (loud > turn.asr_start)
            {
              uint32_t quiet =
                  (uint32_t)((total - loud) / (NY_VOICE_CAPTURE_RATE / 1000));

              if (quiet < event.silence_ms)
                {
                  event.silence_ms = quiet;
                }
            }
        }

      if (event.type == NY_VOICE_EV_LINK_LOST && turn.asr_open)
        {
          turn.asr_open = false;
          turn.asr.wire.request_id = 0;
        }

      nxmutex_lock(&g_voice.lock);
      turn.sm.config.max_listen_ms = g_voice.settings.max_listen_ms;
      nxmutex_unlock(&g_voice.lock);

      ny_voice_sm_step(&turn.sm, &event, &result);

      nxmutex_lock(&g_voice.lock);
      g_voice.state = turn.sm.state;
      nxmutex_unlock(&g_voice.lock);

      ny_voice_turn_act(&turn, &result);
      ny_voice_turn_asr_poll(&turn);
      if (turn.sm.state == NY_VOICE_THINKING || turn.sm.pending ||
          turn.submit_due)
        {
          ny_voice_turn_agent_poll(&turn);
        }

      ny_voice_turn_idle(&turn);
    }

  /* Wake everyone so that they see the stop flag, then collect them. */

  nxmutex_lock(&g_voice.lock);
  g_voice.stopping = true;
  g_voice.state = NY_VOICE_OFF;
  nxmutex_unlock(&g_voice.lock);
  sem_post(&g_voice.play_wake);
  sem_post(&g_voice.eyes_wake);
  if ((started & 1) != 0)
    {
      pthread_join(capture, NULL);
    }

  if ((started & 2) != 0)
    {
      pthread_join(pump, NULL);
    }

  if ((started & 4) != 0)
    {
      pthread_join(play, NULL);
    }

  if ((started & 8) != 0)
    {
      pthread_join(eyes, NULL);
    }

  if (turn.asr_open)
    {
      ny_voice_wire_cancel(&turn.asr.wire, NYAMP_SERVICE_ASR);
    }

  ny_voice_port_close(&turn.asr);

  nxmutex_lock(&g_voice.lock);
  g_voice.running = false;
  g_voice.stopping = false;
  g_voice.play_busy = false;
  g_voice.capture_hold = false;
  nxmutex_unlock(&g_voice.lock);
  return 0;
}

/****************************************************************************
 * Name: ny_voice_status
 ****************************************************************************/

static cJSON *ny_voice_status(void)
{
  static const char *const model_states[] = { "unloaded", "loading", "ready",
                                              "error" };

  struct ny_voice_model_s *models[3];
  static const char *const names[] = { "kws", "asr", "tts" };

  cJSON *root = cJSON_CreateObject();
  cJSON *list;
  cJSON *errors;
  cJSON *mic;
  cJSON *config;
  uint32_t level;
  bool gate;
  bool valid;
  int index;

  if (root == NULL)
    {
      return NULL;
    }

  nxmutex_lock(&g_voice.ring_lock);
  level = g_voice.level;
  gate = g_voice.gate_open;
  nxmutex_unlock(&g_voice.ring_lock);

  models[0] = &g_voice.kws;
  models[1] = &g_voice.asr;
  models[2] = &g_voice.tts;

  nxmutex_lock(&g_voice.lock);
  list = cJSON_AddObjectToObject(root, "models");
  errors = cJSON_AddObjectToObject(root, "errors");
  mic = cJSON_AddObjectToObject(root, "microphone");
  config = cJSON_AddObjectToObject(root, "config");
  valid = list != NULL && errors != NULL && mic != NULL && config != NULL;
  valid =
      valid && cJSON_AddBoolToObject(root, "running", g_voice.running) != NULL;
  valid = valid && cJSON_AddBoolToObject(root, "enabled",
                                         g_voice.settings.enabled) != NULL;
  valid = valid &&
          cJSON_AddStringToObject(
              root, "state", ny_voice_sm_state_name(g_voice.state)) != NULL;
  valid =
      valid && cJSON_AddStringToObject(root, "codec", NY_VOICE_MODE) != NULL;
  valid = valid &&
          cJSON_AddStringToObject(root, "wakeWord", g_voice.wake_word) != NULL;
  valid = valid && cJSON_AddStringToObject(root, "lastTranscript",
                                           g_voice.transcript) != NULL;
  valid = valid &&
          cJSON_AddStringToObject(root, "lastReply", g_voice.reply) != NULL;
  valid = valid &&
          cJSON_AddBoolToObject(root, "streaming", g_voice.streaming) != NULL;
  valid = valid &&
          cJSON_AddNumberToObject(root, "windows", g_voice.windows) != NULL;
  valid = valid && cJSON_AddNumberToObject(root, "detections",
                                           g_voice.detections) != NULL;
  valid =
      valid && cJSON_AddNumberToObject(root, "turns", g_voice.turns) != NULL;
  for (index = 0; index < 3 && valid; index++)
    {
      cJSON *model = cJSON_AddObjectToObject(list, names[index]);

      valid = model != NULL &&
              cJSON_AddStringToObject(model, "state",
                                      model_states[models[index]->state]) !=
                  NULL &&
              cJSON_AddNumberToObject(model, "error", models[index]->error) !=
                  NULL &&
              cJSON_AddNumberToObject(model, "pulled",
                                      (double)models[index]->done) != NULL &&
              cJSON_AddNumberToObject(model, "total",
                                      (double)models[index]->total) != NULL;
    }

  valid = valid &&
          cJSON_AddNumberToObject(errors, "last", g_voice.last_error) != NULL;
  valid = valid && cJSON_AddStringToObject(errors, "text",
                                           g_voice.last_error_text) != NULL;
  valid = valid && cJSON_AddNumberToObject(errors, "capture",
                                           g_voice.capture_error) != NULL;
  valid = valid &&
          cJSON_AddBoolToObject(mic, "open", g_voice.capture_open) != NULL;
  valid = valid && cJSON_AddBoolToObject(mic, "gateOpen", gate) != NULL;
  valid = valid && cJSON_AddNumberToObject(mic, "level", level) != NULL;
  valid =
      valid && cJSON_AddNumberToObject(mic, "claims", g_voice.claims) != NULL;
  valid = valid &&
          cJSON_AddNumberToObject(config, "wakeSensitivity",
                                  g_voice.settings.wake_sensitivity) != NULL;
  valid =
      valid && cJSON_AddNumberToObject(config, "maxListenMs",
                                       g_voice.settings.max_listen_ms) != NULL;
  valid =
      valid && cJSON_AddNumberToObject(config, "replyVoice",
                                       g_voice.settings.reply_voice) != NULL;
  valid =
      valid && cJSON_AddNumberToObject(config, "replySpeed",
                                       g_voice.settings.reply_speed) != NULL;
  nxmutex_unlock(&g_voice.lock);

  if (!valid)
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_voice_start(void)
{
  int ret = nxmutex_lock(&g_voice.lock);

  if (ret < 0)
    {
      return ret;
    }

  if (g_voice.running)
    {
      ret = g_voice.stopping ? -EBUSY : 0;
      nxmutex_unlock(&g_voice.lock);
      return ret;
    }

  sem_init(&g_voice.events_ready, 0, 0);
  sem_init(&g_voice.captured, 0, 0);
  sem_init(&g_voice.play_wake, 0, 0);
  sem_init(&g_voice.eyes_wake, 0, 0);
  ny_voice_ring_init(&g_voice.ring, g_voice_ring, NY_VOICE_RING_SAMPLES);
  g_voice.events_head = 0;
  g_voice.events_count = 0;
  g_voice.state = NY_VOICE_OFF;
  g_voice.stopping = false;
  g_voice.running = true;
  ret = task_create("ny_voice_sm", CONFIG_NYABULA_CORE_VOICE_PRIORITY,
                    CONFIG_NYABULA_CORE_VOICE_STACKSIZE, ny_voice_task, NULL);
  if (ret < 0)
    {
      ret = -errno;
      g_voice.running = false;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&g_voice.lock);
  return ret;
}

int ny_voice_stop(void)
{
  uint64_t deadline = ny_voice_now_ms() + 5000;

  nxmutex_lock(&g_voice.lock);
  g_voice.stopping = g_voice.running;
  nxmutex_unlock(&g_voice.lock);
  while (ny_voice_running())
    {
      if (ny_voice_now_ms() >= deadline)
        {
          return -ETIMEDOUT;
        }

      usleep(20000);
    }

  return 0;
}

bool ny_voice_running(void)
{
  bool running;

  nxmutex_lock(&g_voice.lock);
  running = g_voice.running;
  nxmutex_unlock(&g_voice.lock);
  return running;
}

int ny_voice_speaker_claim(void)
{
  uint64_t deadline = ny_voice_now_ms() + NY_VOICE_CLAIM_MS;

  nxmutex_lock(&g_voice.lock);
  g_voice.claims++;
  nxmutex_unlock(&g_voice.lock);
  for (;;)
    {
      bool open;

      nxmutex_lock(&g_voice.lock);
      open = g_voice.running && g_voice.capture_open;
      nxmutex_unlock(&g_voice.lock);
      if (!open)
        {
          return 0;
        }

      if (ny_voice_now_ms() >= deadline)
        {
          return -ETIMEDOUT;
        }

      usleep(10000);
    }
}

void ny_voice_speaker_release(void)
{
  nxmutex_lock(&g_voice.lock);
  if (g_voice.claims > 0)
    {
      g_voice.claims--;
    }

  nxmutex_unlock(&g_voice.lock);
}

/****************************************************************************
 * Name: ny_voice_request
 *
 * Description:
 *   Panel topics.
 *
 *   voice.status  family  {running, enabled, state, codec, wakeWord,
 *                          lastTranscript, lastReply, streaming, windows,
 *                          detections, turns, models:{kws,asr,tts:{state,
 *                          error, pulled, total}}, errors:{last, text,
 *                          capture}, microphone:{open, gateOpen, level,
 *                          claims}, config:{...}}
 *   voice.enable  owner   {"enabled": bool}, persisted
 *   voice.listen  owner   {} -- push-to-talk: skip the wake word.  Also the
 *                         way to interrupt a reply in the HALF arrangement
 *   voice.cancel  owner   {} -- drop the turn in progress
 *   voice.say     owner   {"text": "..."} -- speak it
 *   voice.config  owner   {"wakeSensitivity"?: 0..100, "maxListenMs"?:
 *                         2000..30000, "replyVoice"?: 0..255,
 *                         "replySpeed"?: 50..200}, persisted
 *
 *   Everything but the status returns the status too.
 *
 ****************************************************************************/

int ny_voice_request(const struct ny_product_caller_s *caller,
                     const char *topic, const cJSON *data, cJSON **result)
{
  bool status = strcmp(topic, "voice.status") == 0;
  enum ny_voice_state_e state;
  int ret = 0;

  if (strncmp(topic, "voice.", 6) != 0)
    {
      return -ENOSYS;
    }

  if (caller->role < (status ? NY_PRODUCT_FAMILY : NY_PRODUCT_OWNER))
    {
      return -EACCES;
    }

  ret = ny_voice_settings_load();
  if (ret < 0)
    {
      return ret;
    }

  state = ny_voice_state();
  if (status)
    {
      /* Nothing to do but report. */
    }
  else if (strcmp(topic, "voice.enable") == 0)
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");

      if (!cJSON_IsBool(enabled))
        {
          return -EINVAL;
        }

      nxmutex_lock(&g_voice.lock);
      g_voice.settings.enabled = cJSON_IsTrue(enabled);
      nxmutex_unlock(&g_voice.lock);
      ret = ny_voice_settings_save();
      if (ret == 0 && cJSON_IsTrue(enabled))
        {
          ret = ny_voice_start();
        }

      if (ret == 0)
        {
          ny_voice_post(cJSON_IsTrue(enabled) ? NY_VOICE_EV_ENABLE
                                              : NY_VOICE_EV_DISABLE,
                        true, false, 0);
        }
    }
  else if (strcmp(topic, "voice.config") == 0)
    {
      const cJSON *wake =
          cJSON_GetObjectItemCaseSensitive(data, "wakeSensitivity");
      const cJSON *listen =
          cJSON_GetObjectItemCaseSensitive(data, "maxListenMs");
      const cJSON *who = cJSON_GetObjectItemCaseSensitive(data, "replyVoice");
      const cJSON *speed =
          cJSON_GetObjectItemCaseSensitive(data, "replySpeed");

      if ((wake != NULL && (!cJSON_IsNumber(wake) || wake->valueint < 0 ||
                            wake->valueint > 100)) ||
          (listen != NULL &&
           (!cJSON_IsNumber(listen) || listen->valueint < 2000 ||
            listen->valueint > 30000)) ||
          (who != NULL && (!cJSON_IsNumber(who) || who->valueint < 0 ||
                           who->valueint > 255)) ||
          (speed != NULL && (!cJSON_IsNumber(speed) || speed->valueint < 50 ||
                             speed->valueint > 200)))
        {
          return -EINVAL;
        }

      nxmutex_lock(&g_voice.lock);
      if (wake != NULL && wake->valueint != g_voice.settings.wake_sensitivity)
        {
          g_voice.settings.wake_sensitivity = wake->valueint;
          g_voice.kws_reload = true;
        }

      if (listen != NULL)
        {
          g_voice.settings.max_listen_ms = (uint32_t)listen->valueint;
        }

      if (who != NULL)
        {
          g_voice.settings.reply_voice = (uint32_t)who->valueint;
        }

      if (speed != NULL)
        {
          g_voice.settings.reply_speed = (uint32_t)speed->valueint;
        }

      nxmutex_unlock(&g_voice.lock);
      ret = ny_voice_settings_save();
    }
  else if (!ny_voice_running() || state == NY_VOICE_OFF)
    {
      return -EPERM; /* voice.enable first. */
    }
  else if (strcmp(topic, "voice.listen") == 0)
    {
      ny_voice_post(NY_VOICE_EV_LISTEN, true, false, 0);
    }
  else if (strcmp(topic, "voice.cancel") == 0)
    {
      ny_voice_post(NY_VOICE_EV_CANCEL, true, false, 0);
    }
  else if (strcmp(topic, "voice.say") == 0)
    {
      const cJSON *text = cJSON_GetObjectItemCaseSensitive(data, "text");

      if (!cJSON_IsString(text) || text->valuestring[0] == '\0' ||
          strlen(text->valuestring) > NY_VOICE_TEXT_MAX)
        {
          return -EINVAL;
        }

      if (state != NY_VOICE_IDLE)
        {
          return -EBUSY;
        }

      nxmutex_lock(&g_voice.lock);
      strlcpy(g_voice.say_text, text->valuestring, sizeof(g_voice.say_text));
      nxmutex_unlock(&g_voice.lock);
      ny_voice_post(NY_VOICE_EV_SAY, true, false, 0);
    }
  else
    {
      return -ENOSYS;
    }

  if (ret < 0)
    {
      return ret;
    }

  *result = ny_voice_status();
  return *result == NULL ? -ENOMEM : 0;
}

#endif /* CONFIG_NYABULA_CORE_VOICE */
