/****************************************************************************
 * app/nyabula_core/ny_product_bt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * The stack half of the Bluetooth service: ZBlue bring-up, visibility,
 * pairing, the device list and the A2DP / AVRCP / HFP signalling.  Audio
 * never passes through here except as bytes handed to ny_product_bt_audio.
 *
 * Three kinds of thread meet in this file:
 *
 *   - Bluetooth host callbacks.  They record what happened under g_bt.lock
 *     and raise a flag; they never wait, touch the store or the codec.
 *   - The product worker, through ny_product_bt_tick(): everything the
 *     callbacks deferred, one AVRCP command per tick at most.
 *   - Request threads, through ny_product_bt_request().
 *
 * No host function is called with g_bt.lock held.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_product_bt.h"
#include "ny_product.h"

#include <errno.h>
#include <string.h>

#ifdef CONFIG_NYABULA_CORE_BT

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/mutex.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/classic/a2dp.h>
#include <zephyr/bluetooth/classic/a2dp_codec_sbc.h>
#include <zephyr/bluetooth/classic/avrcp.h>
#include <zephyr/bluetooth/classic/hfp_hf.h>
#include <zephyr/bluetooth/classic/sdp.h>
#include <zephyr/bluetooth/conn.h>
#ifdef CONFIG_BT_SETTINGS
#include <zephyr/settings/settings.h>
#endif

#include "ny_product_audio.h"
#include "ny_product_bt_audio.h"
#include "ny_product_store.h"
#include "ny_utf8.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_BT_DOMAIN             "bluetooth"
#define NY_BT_SCHEMA             1
#define NY_BT_DEVICES_MAX        8
#define NY_BT_NAME_MAX           64
#define NY_BT_TEXT_MAX           96
#define NY_BT_NUMBER_MAX         32
#define NY_BT_CONTROLLER_WAIT_MS 120000
#define NY_BT_PAIRING_MS         25000 /* The controller gives up at 30 s */
#define NY_BT_DISCOVERABLE_MAX   600
#define NY_BT_CLAIM_MS           600
#define NY_BT_AUTOCONNECT_MS     8000
#define NY_BT_AVRCP_CONNECT_MS   2000
#define NY_BT_PAGE_GIVE_UP_MS    30000
#define NY_BT_VOLUME_STEP        6
#define NY_BT_HFP_GAIN_MAX       15
#define NY_BT_SIG_COMPANY_ID     0x001958

/* Work the callbacks leave for the tick. */

#define NY_BT_WORK_AVRCP_CAPS     (1u << 0)
#define NY_BT_WORK_AVRCP_PLAYBACK (1u << 1)
#define NY_BT_WORK_AVRCP_TRACK    (1u << 2)
#define NY_BT_WORK_AVRCP_ATTRS    (1u << 3)
#define NY_BT_WORK_AVRCP_STATUS   (1u << 4)
#define NY_BT_WORK_AVRCP_RELEASE  (1u << 5)
#define NY_BT_WORK_AVRCP_PAUSE    (1u << 6)
#define NY_BT_WORK_AVRCP_CONNECT  (1u << 7)
#define NY_BT_WORK_A2DP_CONNECT   (1u << 8)
#define NY_BT_WORK_NAME_REQUEST   (1u << 9)
#define NY_BT_WORK_QUIET_FLASH    (1u << 10)
#define NY_BT_WORK_NOTIFY_PAIRING (1u << 11)
#define NY_BT_WORK_NOTIFY_CALL    (1u << 12)
#define NY_BT_WORK_RING           (1u << 13)
#define NY_BT_WORK_AVRCP_ANY                            \
  (NY_BT_WORK_AVRCP_CAPS | NY_BT_WORK_AVRCP_PLAYBACK |  \
   NY_BT_WORK_AVRCP_TRACK | NY_BT_WORK_AVRCP_ATTRS |    \
   NY_BT_WORK_AVRCP_STATUS | NY_BT_WORK_AVRCP_RELEASE | \
   NY_BT_WORK_AVRCP_PAUSE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_bt_state_e
{
  NY_BT_OFF = 0,
  NY_BT_STARTING,
  NY_BT_READY,
  NY_BT_FAILED
};

enum ny_bt_a2dp_e
{
  NY_BT_A2DP_DISCONNECTED = 0,
  NY_BT_A2DP_CONNECTED,
  NY_BT_A2DP_OPEN,
  NY_BT_A2DP_STREAMING
};

enum ny_bt_call_e
{
  NY_BT_CALL_IDLE = 0,
  NY_BT_CALL_INCOMING,
  NY_BT_CALL_DIALING,
  NY_BT_CALL_ALERTING,
  NY_BT_CALL_ACTIVE,
  NY_BT_CALL_HELD
};

struct ny_bt_device_s
{
  char address[BT_ADDR_STR_LEN];
  char name[NY_BT_NAME_MAX];
  uint64_t last_connected; /* Wall clock, ms; 0 when never */
  bool bonded;             /* Scratch for bt.devices */
};

struct ny_bt_s
{
  mutex_t lock;
  pthread_t thread;
  bool thread_started;
  bool stop;
  enum ny_bt_state_e state;
  int error;
  uint32_t work;

  char name[33];
  char address[BT_ADDR_STR_LEN];
  bool discoverable;
  uint64_t discoverable_until;

  /* One pairing at a time waits for the owner. */

  struct bt_conn *pairing;
  char pairing_address[BT_ADDR_STR_LEN];
  uint64_t pairing_deadline;
  bool pairing_passkey_style;

  /* The one phone. */

  struct bt_conn *acl;
  bool acl_initiated;
  char peer_address[BT_ADDR_STR_LEN];
  uint64_t acl_since;
  uint64_t acl_attempt_until; /* An outgoing call is given up after this */
  uint64_t autoconnect_at;

  struct bt_a2dp *a2dp;
  enum ny_bt_a2dp_e a2dp_state;
  uint32_t a2dp_rate;
  uint8_t a2dp_channels;

  struct bt_avrcp_ct *ct;
  struct bt_avrcp_tg *tg;
  uint8_t tid;
  uint16_t avrcp_events; /* Bit n: the phone supports event n */
  uint8_t play_status;
  bool play_status_known;
  char title[NY_BT_TEXT_MAX];
  char artist[NY_BT_TEXT_MAX];
  uint8_t release_opid;
  bool volume_registered; /* The phone waits for our VOLUME_CHANGED */
  uint8_t volume_tid;
  int volume_absolute; /* Last value exchanged, 0..127, or -1 */
  int volume_pending;  /* 0..100 asked by the phone, or -1 */
  int volume_seen;     /* Local volume at the last look */

  struct bt_hfp_hf *hf;
  struct bt_hfp_hf_call *call;
  enum ny_bt_call_e call_state;
  char number[NY_BT_NUMBER_MAX];
  bool inband_ring;
  uint8_t codec_id;
  bool sco_up;
  int hfp_service;
  int hfp_signal;
  int hfp_battery;
  uint32_t call_serial;

  struct ny_bt_device_s devices[NY_BT_DEVICES_MAX];
  bool devices_loaded;
  bool devices_dirty;
  uint64_t devices_retry;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ny_bt_work(uint32_t work);
static void ny_bt_address(const struct bt_conn *conn, char *out);
static struct ny_bt_device_s *ny_bt_device(const char *address, bool create);
static void ny_bt_text(char *out, size_t capacity, const uint8_t *text,
                       size_t length);
static int ny_bt_volume_to_absolute(int volume);
static int ny_bt_absolute_to_volume(int absolute);

static void ny_bt_connected(struct bt_conn *conn, uint8_t err);
static void ny_bt_disconnected(struct bt_conn *conn, uint8_t reason);
static void ny_bt_pairing_request(struct bt_conn *conn, bool passkey_style);
static void ny_bt_pairing_confirm(struct bt_conn *conn);
static void ny_bt_passkey_confirm(struct bt_conn *conn, unsigned int passkey);
static void ny_bt_auth_cancel(struct bt_conn *conn);
static void ny_bt_pairing_complete(struct bt_conn *conn, bool bonded);
static void ny_bt_pairing_failed(struct bt_conn *conn,
                                 enum bt_security_err reason);
static void ny_bt_remote_name(const bt_addr_t *address, const char *name,
                              uint8_t status);

static int ny_bt_a2dp_parse(const struct bt_a2dp_codec_cfg *config,
                            uint8_t *error);
static void ny_bt_a2dp_connected(struct bt_a2dp *a2dp, int err);
static void ny_bt_a2dp_disconnected(struct bt_a2dp *a2dp);
static int ny_bt_a2dp_config_req(struct bt_a2dp *a2dp, struct bt_a2dp_ep *ep,
                                 struct bt_a2dp_codec_cfg *config,
                                 struct bt_a2dp_stream **stream,
                                 uint8_t *error);
static int ny_bt_a2dp_reconfig_req(struct bt_a2dp_stream *stream,
                                   struct bt_a2dp_codec_cfg *config,
                                   uint8_t *error);
static int ny_bt_a2dp_accept_req(struct bt_a2dp_stream *stream,
                                 uint8_t *error);
static void ny_bt_a2dp_established(struct bt_a2dp_stream *stream);
static void ny_bt_a2dp_released(struct bt_a2dp_stream *stream);
static void ny_bt_a2dp_started(struct bt_a2dp_stream *stream);
static void ny_bt_a2dp_suspended(struct bt_a2dp_stream *stream);
static void ny_bt_a2dp_recv(struct bt_a2dp_stream *stream, struct net_buf *buf,
                            uint16_t sequence, uint32_t timestamp);

static uint8_t ny_bt_tid(void);
static void ny_bt_ct_connected(struct bt_conn *conn, struct bt_avrcp_ct *ct);
static void ny_bt_ct_disconnected(struct bt_avrcp_ct *ct);
static void ny_bt_ct_get_caps(struct bt_avrcp_ct *ct, uint8_t tid,
                              uint8_t status, struct net_buf *buf);
static void
ny_bt_ct_passthrough_rsp(struct bt_avrcp_ct *ct, uint8_t tid,
                         bt_avrcp_rsp_t result,
                         const struct bt_avrcp_passthrough_rsp *rsp);
static void ny_bt_ct_event(uint8_t event,
                           const struct bt_avrcp_event_data *data);
static void ny_bt_ct_notification(struct bt_avrcp_ct *ct, uint8_t tid,
                                  uint8_t status, uint8_t event,
                                  struct bt_avrcp_event_data *data);
static void ny_bt_ct_changed(struct bt_avrcp_ct *ct, uint8_t event,
                             struct bt_avrcp_event_data *data);
static void ny_bt_ct_element_attrs(struct bt_avrcp_ct *ct, uint8_t tid,
                                   uint8_t status, struct net_buf *buf);
static void ny_bt_ct_play_status(struct bt_avrcp_ct *ct, uint8_t tid,
                                 uint8_t status, struct net_buf *buf);
static void ny_bt_tg_connected(struct bt_conn *conn, struct bt_avrcp_tg *tg);
static void ny_bt_tg_disconnected(struct bt_avrcp_tg *tg);
static void ny_bt_tg_unit_info(struct bt_avrcp_tg *tg, uint8_t tid);
static void ny_bt_tg_subunit_info(struct bt_avrcp_tg *tg, uint8_t tid);
static void ny_bt_tg_get_caps(struct bt_avrcp_tg *tg, uint8_t tid,
                              uint8_t cap_id);
static void ny_bt_tg_register(struct bt_avrcp_tg *tg, uint8_t tid,
                              uint8_t event, uint32_t interval);
static void ny_bt_tg_passthrough(struct bt_avrcp_tg *tg, uint8_t tid,
                                 struct net_buf *buf);
static void ny_bt_tg_volume(struct bt_avrcp_tg *tg, uint8_t tid,
                            uint8_t absolute);
static int ny_bt_passthrough(uint8_t opid);

static void ny_bt_call_set(struct bt_hfp_hf_call *call,
                           enum ny_bt_call_e state);
static void ny_bt_hf_connected(struct bt_conn *conn, struct bt_hfp_hf *hf);
static void ny_bt_hf_disconnected(struct bt_hfp_hf *hf);
static int ny_bt_sco_send(const uint8_t *data, size_t size);
static void ny_bt_hf_sco_connected(struct bt_hfp_hf *hf, struct bt_conn *sco);
static void ny_bt_hf_sco_recv(struct bt_hfp_hf *hf, const uint8_t *data,
                              size_t size, uint8_t packet_status);
static void ny_bt_hf_sco_disconnected(struct bt_conn *sco, uint8_t reason);
static void ny_bt_hf_service(struct bt_hfp_hf *hf, uint32_t value);
static void ny_bt_hf_outgoing(struct bt_hfp_hf *hf,
                              struct bt_hfp_hf_call *call);
static void ny_bt_hf_remote_ringing(struct bt_hfp_hf_call *call);
static void ny_bt_hf_incoming(struct bt_hfp_hf *hf,
                              struct bt_hfp_hf_call *call);
static void ny_bt_hf_accept(struct bt_hfp_hf_call *call);
static void ny_bt_hf_ended(struct bt_hfp_hf_call *call);
static void ny_bt_hf_held(struct bt_hfp_hf_call *call);
static void ny_bt_hf_signal(struct bt_hfp_hf *hf, uint32_t value);
static void ny_bt_hf_battery(struct bt_hfp_hf *hf, uint32_t value);
static void ny_bt_hf_ring(struct bt_hfp_hf_call *call);
static void ny_bt_hf_clip(struct bt_hfp_hf_call *call, char *number,
                          uint8_t type);
static void ny_bt_hf_vgs(struct bt_hfp_hf *hf, uint8_t gain);
static void ny_bt_hf_inband(struct bt_hfp_hf *hf, bool inband);
static void ny_bt_hf_codec(struct bt_hfp_hf *hf, uint8_t id);

static void ny_bt_apply_name(void);
static void *ny_bt_bringup(void *arg);
static void ny_bt_devices_load(void);
static void ny_bt_devices_save(void);
static void ny_bt_bond_mark(const struct bt_bond_info *info, void *arg);
static int ny_bt_discoverable(bool enable, int seconds);
static void ny_bt_tick_avrcp(uint32_t work);
static void ny_bt_tick_volume(void);
static cJSON *ny_bt_status(void);
static int ny_bt_devices(cJSON **result);
static int ny_bt_target(const cJSON *data, bool required, bt_addr_t *address,
                        char *text);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ny_bt_s g_bt = {
  .lock = NXMUTEX_INITIALIZER,
  .volume_absolute = -1,
  .volume_pending = -1,
  .volume_seen = -1,
  .hfp_service = -1,
  .hfp_signal = -1,
  .hfp_battery = -1,
};

/* The A2DP sink record is the application's to publish; AVRCP and HFP
 * publish their own.
 */

static struct bt_sdp_attribute g_bt_a2dp_sink_attrs[] = {
  BT_SDP_NEW_SERVICE,
  BT_SDP_LIST(
      BT_SDP_ATTR_SVCLASS_ID_LIST, BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
      BT_SDP_DATA_ELEM_LIST({ BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
                              BT_SDP_ARRAY_16(BT_SDP_AUDIO_SINK_SVCLASS) }, )),
  BT_SDP_LIST(
      BT_SDP_ATTR_PROTO_DESC_LIST, BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 16),
      BT_SDP_DATA_ELEM_LIST(
          { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
            BT_SDP_DATA_ELEM_LIST({ BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
                                    BT_SDP_ARRAY_16(BT_SDP_PROTO_L2CAP) },
                                  { BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                                    BT_SDP_ARRAY_16(BT_UUID_AVDTP_VAL) }, ) },
          { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
            BT_SDP_DATA_ELEM_LIST({ BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
                                    BT_SDP_ARRAY_16(BT_UUID_AVDTP_VAL) },
                                  {
                                      BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                                      BT_SDP_ARRAY_16(0x0103U) /* AVDTP 1.3 */
                                  }, ) }, )),
  BT_SDP_LIST(BT_SDP_ATTR_PROFILE_DESC_LIST,
              BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
              BT_SDP_DATA_ELEM_LIST(
                  { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                    BT_SDP_DATA_ELEM_LIST(
                        { BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
                          BT_SDP_ARRAY_16(BT_SDP_ADVANCED_AUDIO_SVCLASS) },
                        {
                            BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                            BT_SDP_ARRAY_16(0x0103U) /* A2DP 1.3 */
                        }, ) }, )),
  BT_SDP_SERVICE_NAME("Nyabula speaker"),
  BT_SDP_SUPPORTED_FEATURES(0x0001U), /* Speaker */
};

static struct bt_sdp_record g_bt_a2dp_sink_record =
    BT_SDP_RECORD(g_bt_a2dp_sink_attrs);

/* Everything SBC can be, at the two rates the codec runs without
 * resampling.  Bitpool 53 is the "high quality" ceiling phones use.
 */

BT_A2DP_SBC_SINK_EP(g_bt_sbc_endpoint,
                    A2DP_SBC_SAMP_FREQ_44100 | A2DP_SBC_SAMP_FREQ_48000,
                    A2DP_SBC_CH_MODE_MONO | A2DP_SBC_CH_MODE_DUAL |
                        A2DP_SBC_CH_MODE_STEREO | A2DP_SBC_CH_MODE_JOINT,
                    A2DP_SBC_BLK_LEN_4 | A2DP_SBC_BLK_LEN_8 |
                        A2DP_SBC_BLK_LEN_12 | A2DP_SBC_BLK_LEN_16,
                    A2DP_SBC_SUBBAND_4 | A2DP_SBC_SUBBAND_8,
                    A2DP_SBC_ALLOC_MTHD_SNR | A2DP_SBC_ALLOC_MTHD_LOUDNESS, 2U,
                    53U, false);

static struct bt_a2dp_stream g_bt_sbc_stream;

static struct bt_a2dp_stream_ops g_bt_stream_ops = {
  .established = ny_bt_a2dp_established,
  .released = ny_bt_a2dp_released,
  .started = ny_bt_a2dp_started,
  .suspended = ny_bt_a2dp_suspended,
  .recv = ny_bt_a2dp_recv,
};

static struct bt_a2dp_cb g_bt_a2dp_cb = {
  .connected = ny_bt_a2dp_connected,
  .disconnected = ny_bt_a2dp_disconnected,
  .config_req = ny_bt_a2dp_config_req,
  .reconfig_req = ny_bt_a2dp_reconfig_req,
  .establish_req = ny_bt_a2dp_accept_req,
  .release_req = ny_bt_a2dp_accept_req,
  .start_req = ny_bt_a2dp_accept_req,
  .suspend_req = ny_bt_a2dp_accept_req,
  .abort_req = ny_bt_a2dp_accept_req,
};

static const struct bt_avrcp_ct_cb g_bt_ct_cb = {
  .connected = ny_bt_ct_connected,
  .disconnected = ny_bt_ct_disconnected,
  .get_caps = ny_bt_ct_get_caps,
  .passthrough_rsp = ny_bt_ct_passthrough_rsp,
  .notification = ny_bt_ct_notification,
  .get_element_attrs = ny_bt_ct_element_attrs,
  .get_play_status = ny_bt_ct_play_status,
};

static const struct bt_avrcp_tg_cb g_bt_tg_cb = {
  .connected = ny_bt_tg_connected,
  .disconnected = ny_bt_tg_disconnected,
  .unit_info_req = ny_bt_tg_unit_info,
  .subunit_info_req = ny_bt_tg_subunit_info,
  .get_caps = ny_bt_tg_get_caps,
  .register_notification = ny_bt_tg_register,
  .passthrough_req = ny_bt_tg_passthrough,
  .set_absolute_volume = ny_bt_tg_volume,
};

static struct bt_hfp_hf_cb g_bt_hf_cb = {
  .connected = ny_bt_hf_connected,
  .disconnected = ny_bt_hf_disconnected,
  .sco_connected = ny_bt_hf_sco_connected,
  .sco_recv = ny_bt_hf_sco_recv,
  .sco_disconnected = ny_bt_hf_sco_disconnected,
  .service = ny_bt_hf_service,
  .outgoing = ny_bt_hf_outgoing,
  .remote_ringing = ny_bt_hf_remote_ringing,
  .incoming = ny_bt_hf_incoming,
  .accept = ny_bt_hf_accept,
  .reject = ny_bt_hf_ended,
  .terminate = ny_bt_hf_ended,
  .held = ny_bt_hf_held,
  .retrieve = ny_bt_hf_accept,
  .signal = ny_bt_hf_signal,
  .battery = ny_bt_hf_battery,
  .ring_indication = ny_bt_hf_ring,
  .clip = ny_bt_hf_clip,
  .vgs = ny_bt_hf_vgs,
  .inband_ring = ny_bt_hf_inband,
  .codec_negotiate = ny_bt_hf_codec,
};

static struct bt_conn_cb g_bt_conn_cb = {
  .connected = ny_bt_connected,
  .disconnected = ny_bt_disconnected,
};

/* Neither a display nor a keyboard as far as the peer can tell: that makes
 * every pairing "Just Works", and the owner's yes on the panel is what
 * stands in for the missing comparison.  Which of the two callbacks the host
 * uses for it depends on its configuration, so both are given.
 */

static const struct bt_conn_auth_cb g_bt_auth_cb = {
  .passkey_confirm = ny_bt_passkey_confirm,
  .pairing_confirm = ny_bt_pairing_confirm,
  .cancel = ny_bt_auth_cancel,
};

static struct bt_conn_auth_info_cb g_bt_auth_info_cb = {
  .pairing_complete = ny_bt_pairing_complete,
  .pairing_failed = ny_bt_pairing_failed,
};

static const char *const g_bt_state_names[] = { "off", "starting", "ready",
                                                "failed" };

static const char *const g_bt_a2dp_names[] = { "disconnected", "connected",
                                               "open", "streaming" };

static const char *const g_bt_call_names[] = { "idle",    "incoming",
                                               "dialing", "alerting",
                                               "active",  "held" };

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_bt_work
 ****************************************************************************/

static void ny_bt_work(uint32_t work)
{
  nxmutex_lock(&g_bt.lock);
  g_bt.work |= work;
  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: ny_bt_address
 ****************************************************************************/

static void ny_bt_address(const struct bt_conn *conn, char *out)
{
  const bt_addr_t *address = bt_conn_get_dst_br(conn);

  out[0] = '\0';
  if (address != NULL)
    {
      bt_addr_to_str(address, out, BT_ADDR_STR_LEN);
    }
}

/****************************************************************************
 * Name: ny_bt_device
 *
 * Description:
 *   The list entry of an address, made on request in place of the entry
 *   that has been away longest.  Called with g_bt.lock held.
 *
 ****************************************************************************/

static struct ny_bt_device_s *ny_bt_device(const char *address, bool create)
{
  struct ny_bt_device_s *slot = NULL;
  int index;

  for (index = 0; index < NY_BT_DEVICES_MAX; index++)
    {
      struct ny_bt_device_s *device = &g_bt.devices[index];

      if (device->address[0] == '\0')
        {
          if (slot == NULL || slot->address[0] != '\0')
            {
              slot = device;
            }
        }
      else if (strcasecmp(device->address, address) == 0)
        {
          return device;
        }
      else if (slot == NULL || (slot->address[0] != '\0' &&
                                device->last_connected < slot->last_connected))
        {
          slot = device;
        }
    }

  if (!create)
    {
      return NULL;
    }

  memset(slot, 0, sizeof(*slot));
  snprintf(slot->address, sizeof(slot->address), "%s", address);
  g_bt.devices_dirty = true;
  return slot;
}

/****************************************************************************
 * Name: ny_bt_text
 *
 * Description:
 *   Text from the phone, for JSON: cut at a character boundary, dropped
 *   altogether when it is not UTF-8.
 *
 ****************************************************************************/

static void ny_bt_text(char *out, size_t capacity, const uint8_t *text,
                       size_t length)
{
  if (length >= capacity)
    {
      length = capacity - 1;
      while (length > 0 && (text[length] & 0xc0) == 0x80)
        {
          length--;
        }
    }

  out[0] = '\0';
  if (length > 0 && memchr(text, '\0', length) == NULL &&
      ny_utf8_valid(text, length))
    {
      memcpy(out, text, length);
      out[length] = '\0';
    }
}

/****************************************************************************
 * Name: ny_bt_volume_to_absolute / ny_bt_absolute_to_volume
 ****************************************************************************/

static int ny_bt_volume_to_absolute(int volume)
{
  return (volume * BT_AVRCP_MAX_ABSOLUTE_VOLUME + 50) / 100;
}

static int ny_bt_absolute_to_volume(int absolute)
{
  return (absolute * 100 + BT_AVRCP_MAX_ABSOLUTE_VOLUME / 2) /
         BT_AVRCP_MAX_ABSOLUTE_VOLUME;
}

/****************************************************************************
 * Name: ny_bt_connected
 ****************************************************************************/

static void ny_bt_connected(struct bt_conn *conn, uint8_t err)
{
  struct bt_conn_info info;
  char address[BT_ADDR_STR_LEN];

  if (bt_conn_get_info(conn, &info) < 0 || info.type != BT_CONN_TYPE_BR)
    {
      return;
    }

  ny_bt_address(conn, address);
  nxmutex_lock(&g_bt.lock);
  if (err != 0)
    {
      if (g_bt.acl == conn)
        {
          bt_conn_unref(g_bt.acl);
          g_bt.acl = NULL;
          g_bt.acl_initiated = false;
        }

      nxmutex_unlock(&g_bt.lock);
      syslog(LOG_INFO, "nybt: connect to %s failed: 0x%02x\n", address, err);
      return;
    }

  if (g_bt.acl == NULL)
    {
      g_bt.acl = bt_conn_ref(conn);
    }

  if (g_bt.acl == conn)
    {
      struct ny_bt_device_s *device;

      snprintf(g_bt.peer_address, sizeof(g_bt.peer_address), "%s", address);
      g_bt.acl_since = ny_product_time_ms(true);
      g_bt.acl_attempt_until = 0;
      device = ny_bt_device(address, true);
      device->last_connected = ny_product_time_ms(false);
      g_bt.devices_dirty = true;
      g_bt.work |= NY_BT_WORK_NAME_REQUEST;
      if (g_bt.acl_initiated)
        {
          g_bt.work |= NY_BT_WORK_A2DP_CONNECT;
        }
    }

  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: connected %s\n", address);
}

/****************************************************************************
 * Name: ny_bt_disconnected
 ****************************************************************************/

static void ny_bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
  struct bt_conn *acl = NULL;
  struct bt_conn *pairing = NULL;

  nxmutex_lock(&g_bt.lock);
  if (g_bt.pairing == conn)
    {
      pairing = g_bt.pairing;
      g_bt.pairing = NULL;
    }

  if (g_bt.acl == conn)
    {
      acl = g_bt.acl;
      g_bt.acl = NULL;
      g_bt.acl_initiated = false;
      g_bt.acl_attempt_until = 0;
      g_bt.peer_address[0] = '\0';
      g_bt.work &= ~(NY_BT_WORK_AVRCP_CONNECT | NY_BT_WORK_A2DP_CONNECT |
                     NY_BT_WORK_NAME_REQUEST);
    }

  nxmutex_unlock(&g_bt.lock);
  if (acl != NULL)
    {
      syslog(LOG_INFO, "nybt: disconnected, reason 0x%02x\n", reason);
      bt_conn_unref(acl);
    }

  if (pairing != NULL)
    {
      bt_conn_unref(pairing);
    }
}

/****************************************************************************
 * Name: ny_bt_pairing_request
 *
 * Description:
 *   A device wants to pair.  Outside the window the owner opened with
 *   bt.discoverable the answer is no without asking anybody; inside it the
 *   request waits for bt.pair.confirm.
 *
 ****************************************************************************/

static void ny_bt_pairing_request(struct bt_conn *conn, bool passkey_style)
{
  bool refuse;

  nxmutex_lock(&g_bt.lock);
  refuse = !g_bt.discoverable || g_bt.pairing != NULL;
  if (!refuse)
    {
      g_bt.pairing = bt_conn_ref(conn);
      g_bt.pairing_passkey_style = passkey_style;
      g_bt.pairing_deadline = ny_product_time_ms(true) + NY_BT_PAIRING_MS;
      ny_bt_address(conn, g_bt.pairing_address);
      g_bt.work |= NY_BT_WORK_NOTIFY_PAIRING;
    }

  nxmutex_unlock(&g_bt.lock);
  if (refuse)
    {
      syslog(LOG_INFO, "nybt: pairing refused: not in pairing mode\n");
      bt_conn_auth_cancel(conn);
    }
}

static void ny_bt_pairing_confirm(struct bt_conn *conn)
{
  ny_bt_pairing_request(conn, false);
}

static void ny_bt_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
  (void)passkey;
  ny_bt_pairing_request(conn, true);
}

/****************************************************************************
 * Name: ny_bt_auth_cancel
 ****************************************************************************/

static void ny_bt_auth_cancel(struct bt_conn *conn)
{
  struct bt_conn *pairing = NULL;

  nxmutex_lock(&g_bt.lock);
  if (g_bt.pairing == conn)
    {
      pairing = g_bt.pairing;
      g_bt.pairing = NULL;
    }

  nxmutex_unlock(&g_bt.lock);
  if (pairing != NULL)
    {
      bt_conn_unref(pairing);
    }
}

/****************************************************************************
 * Name: ny_bt_pairing_complete / ny_bt_pairing_failed
 ****************************************************************************/

static void ny_bt_pairing_complete(struct bt_conn *conn, bool bonded)
{
  char address[BT_ADDR_STR_LEN];

  ny_bt_address(conn, address);
  syslog(LOG_INFO, "nybt: paired %s bonded=%d\n", address, bonded);
  ny_bt_auth_cancel(conn);
  if (address[0] != '\0')
    {
      nxmutex_lock(&g_bt.lock);
      ny_bt_device(address, true);
      nxmutex_unlock(&g_bt.lock);
    }
}

static void ny_bt_pairing_failed(struct bt_conn *conn,
                                 enum bt_security_err reason)
{
  syslog(LOG_INFO, "nybt: pairing failed: %d\n", (int)reason);
  ny_bt_auth_cancel(conn);
}

/****************************************************************************
 * Name: ny_bt_remote_name
 ****************************************************************************/

static void ny_bt_remote_name(const bt_addr_t *address, const char *name,
                              uint8_t status)
{
  char text[BT_ADDR_STR_LEN];
  struct ny_bt_device_s *device;

  if (status != 0 || address == NULL || name == NULL)
    {
      return;
    }

  bt_addr_to_str(address, text, sizeof(text));
  nxmutex_lock(&g_bt.lock);
  device = ny_bt_device(text, false);
  if (device != NULL)
    {
      char clean[NY_BT_NAME_MAX];

      ny_bt_text(clean, sizeof(clean), (const uint8_t *)name, strlen(name));
      if (clean[0] != '\0' && strcmp(clean, device->name) != 0)
        {
          memcpy(device->name, clean, sizeof(device->name));
          g_bt.devices_dirty = true;
        }
    }

  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: ny_bt_a2dp_parse
 *
 * Description:
 *   Take the stream format out of an SBC configuration.  A configuration
 *   names exactly one value where the capabilities named several.
 *
 ****************************************************************************/

static int ny_bt_a2dp_parse(const struct bt_a2dp_codec_cfg *config,
                            uint8_t *error)
{
  const uint8_t *ie;
  uint32_t rate;
  uint8_t channels;
  uint16_t blocks;
  uint16_t subbands;

  if (config == NULL || config->codec_config == NULL ||
      config->codec_config->len < BT_A2DP_SBC_IE_LENGTH)
    {
      *error = BT_A2DP_INVALID_CODEC_PARAMETER;
      return -EINVAL;
    }

  ie = config->codec_config->codec_ie;
  switch (ie[0] & 0xf0)
    {
      case A2DP_SBC_SAMP_FREQ_44100:
        rate = 44100;
        break;

      case A2DP_SBC_SAMP_FREQ_48000:
        rate = 48000;
        break;

      case A2DP_SBC_SAMP_FREQ_16000:
      case A2DP_SBC_SAMP_FREQ_32000:
        *error = BT_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY;
        return -ENOTSUP;

      default:
        *error = BT_A2DP_INVALID_SAMPLING_FREQUENCY;
        return -EINVAL;
    }

  switch (ie[0] & 0x0f)
    {
      case A2DP_SBC_CH_MODE_MONO:
        channels = 1;
        break;

      case A2DP_SBC_CH_MODE_DUAL:
      case A2DP_SBC_CH_MODE_STEREO:
      case A2DP_SBC_CH_MODE_JOINT:
        channels = 2;
        break;

      default:
        *error = BT_A2DP_INVALID_CHANNEL_MODE;
        return -EINVAL;
    }

  switch (ie[1] & 0xf0)
    {
      case A2DP_SBC_BLK_LEN_4:
        blocks = 4;
        break;

      case A2DP_SBC_BLK_LEN_8:
        blocks = 8;
        break;

      case A2DP_SBC_BLK_LEN_12:
        blocks = 12;
        break;

      case A2DP_SBC_BLK_LEN_16:
        blocks = 16;
        break;

      default:
        *error = BT_A2DP_INVALID_BLOCK_LENGTH;
        return -EINVAL;
    }

  switch (ie[1] & 0x0c)
    {
      case A2DP_SBC_SUBBAND_4:
        subbands = 4;
        break;

      case A2DP_SBC_SUBBAND_8:
        subbands = 8;
        break;

      default:
        *error = BT_A2DP_INVALID_SUBBANDS;
        return -EINVAL;
    }

  ny_bt_audio_media_configure(rate, channels, blocks * subbands);
  nxmutex_lock(&g_bt.lock);
  g_bt.a2dp_rate = rate;
  g_bt.a2dp_channels = channels;
  nxmutex_unlock(&g_bt.lock);
  *error = 0;
  return 0;
}

/****************************************************************************
 * Name: ny_bt_a2dp_connected / ny_bt_a2dp_disconnected
 ****************************************************************************/

static void ny_bt_a2dp_connected(struct bt_a2dp *a2dp, int err)
{
  if (err != 0)
    {
      syslog(LOG_INFO, "nybt: a2dp connect failed: %d\n", err);
      return;
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.a2dp = a2dp;
  g_bt.a2dp_state = NY_BT_A2DP_CONNECTED;
  if (g_bt.ct == NULL)
    {
      /* Most phones bring AVRCP up themselves right after; ask only when
       * that has not happened a little later.
       */

      g_bt.work |= NY_BT_WORK_AVRCP_CONNECT;
    }

  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: a2dp connected\n");
}

static void ny_bt_a2dp_disconnected(struct bt_a2dp *a2dp)
{
  ny_bt_audio_media_stop();
  nxmutex_lock(&g_bt.lock);
  if (g_bt.a2dp == a2dp)
    {
      g_bt.a2dp = NULL;
    }

  g_bt.a2dp_state = NY_BT_A2DP_DISCONNECTED;
  g_bt.a2dp_rate = 0;
  g_bt.a2dp_channels = 0;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: a2dp disconnected\n");
}

/****************************************************************************
 * Name: ny_bt_a2dp_config_req / ny_bt_a2dp_reconfig_req
 ****************************************************************************/

static int ny_bt_a2dp_config_req(struct bt_a2dp *a2dp, struct bt_a2dp_ep *ep,
                                 struct bt_a2dp_codec_cfg *config,
                                 struct bt_a2dp_stream **stream,
                                 uint8_t *error)
{
  int ret;

  (void)a2dp;
  (void)ep;
  ret = ny_bt_a2dp_parse(config, error);
  if (ret < 0)
    {
      return ret;
    }

  bt_a2dp_stream_cb_register(&g_bt_sbc_stream, &g_bt_stream_ops);
  *stream = &g_bt_sbc_stream;
  return 0;
}

static int ny_bt_a2dp_reconfig_req(struct bt_a2dp_stream *stream,
                                   struct bt_a2dp_codec_cfg *config,
                                   uint8_t *error)
{
  (void)stream;
  return ny_bt_a2dp_parse(config, error);
}

/****************************************************************************
 * Name: ny_bt_a2dp_accept_req
 *
 * Description:
 *   Open, start, suspend, close and abort are the phone's to decide.
 *
 ****************************************************************************/

static int ny_bt_a2dp_accept_req(struct bt_a2dp_stream *stream, uint8_t *error)
{
  (void)stream;
  *error = 0;
  return 0;
}

/****************************************************************************
 * Name: A2DP stream callbacks
 ****************************************************************************/

static void ny_bt_a2dp_established(struct bt_a2dp_stream *stream)
{
  (void)stream;
  nxmutex_lock(&g_bt.lock);
  g_bt.a2dp_state = NY_BT_A2DP_OPEN;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_a2dp_released(struct bt_a2dp_stream *stream)
{
  (void)stream;
  ny_bt_audio_media_stop();
  nxmutex_lock(&g_bt.lock);
  if (g_bt.a2dp_state != NY_BT_A2DP_DISCONNECTED)
    {
      g_bt.a2dp_state = NY_BT_A2DP_CONNECTED;
    }

  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_a2dp_started(struct bt_a2dp_stream *stream)
{
  (void)stream;
  ny_bt_audio_media_start();
  nxmutex_lock(&g_bt.lock);
  g_bt.a2dp_state = NY_BT_A2DP_STREAMING;

  /* The newest source wins the speaker: music from flash stops.  The alert
   * chime is not music and is left alone.
   */

  g_bt.work |= NY_BT_WORK_QUIET_FLASH;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: a2dp streaming\n");
}

static void ny_bt_a2dp_suspended(struct bt_a2dp_stream *stream)
{
  (void)stream;
  ny_bt_audio_media_stop();
  nxmutex_lock(&g_bt.lock);
  g_bt.a2dp_state = NY_BT_A2DP_OPEN;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: a2dp suspended\n");
}

static void ny_bt_a2dp_recv(struct bt_a2dp_stream *stream, struct net_buf *buf,
                            uint16_t sequence, uint32_t timestamp)
{
  (void)stream;
  (void)sequence;
  (void)timestamp;
  ny_bt_audio_media_packet(buf->data, buf->len);
}

/****************************************************************************
 * Name: ny_bt_tid
 ****************************************************************************/

static uint8_t ny_bt_tid(void)
{
  uint8_t tid;

  nxmutex_lock(&g_bt.lock);
  tid = g_bt.tid;
  g_bt.tid = (g_bt.tid + 1) & 0x0f;
  nxmutex_unlock(&g_bt.lock);
  return tid;
}

/****************************************************************************
 * Name: AVRCP controller callbacks (we control the phone's player)
 ****************************************************************************/

static void ny_bt_ct_connected(struct bt_conn *conn, struct bt_avrcp_ct *ct)
{
  (void)conn;
  nxmutex_lock(&g_bt.lock);
  g_bt.ct = ct;
  g_bt.avrcp_events = 0;
  g_bt.play_status_known = false;
  g_bt.title[0] = '\0';
  g_bt.artist[0] = '\0';
  g_bt.work &= ~(NY_BT_WORK_AVRCP_ANY | NY_BT_WORK_AVRCP_CONNECT);
  g_bt.work |= NY_BT_WORK_AVRCP_CAPS;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: avrcp controller connected\n");
}

static void ny_bt_ct_disconnected(struct bt_avrcp_ct *ct)
{
  nxmutex_lock(&g_bt.lock);
  if (g_bt.ct == ct)
    {
      g_bt.ct = NULL;
    }

  g_bt.work &= ~NY_BT_WORK_AVRCP_ANY;
  g_bt.play_status_known = false;
  g_bt.title[0] = '\0';
  g_bt.artist[0] = '\0';
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_ct_get_caps(struct bt_avrcp_ct *ct, uint8_t tid,
                              uint8_t status, struct net_buf *buf)
{
  const struct bt_avrcp_get_caps_rsp *rsp;
  uint16_t events = 0;
  uint8_t index;

  (void)ct;
  (void)tid;
  if (status != BT_AVRCP_STATUS_SUCCESS || buf == NULL ||
      buf->len < sizeof(*rsp))
    {
      return;
    }

  rsp = (const struct bt_avrcp_get_caps_rsp *)buf->data;
  if (rsp->cap_id != BT_AVRCP_CAP_EVENTS_SUPPORTED ||
      buf->len < sizeof(*rsp) + rsp->cap_cnt)
    {
      return;
    }

  for (index = 0; index < rsp->cap_cnt; index++)
    {
      if (rsp->cap[index] < 16)
        {
          events |= (uint16_t)(1u << rsp->cap[index]);
        }
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.avrcp_events = events;
  if (events & (1u << BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED))
    {
      g_bt.work |= NY_BT_WORK_AVRCP_PLAYBACK;
    }

  if (events & (1u << BT_AVRCP_EVT_TRACK_CHANGED))
    {
      g_bt.work |= NY_BT_WORK_AVRCP_TRACK;
    }

  g_bt.work |= NY_BT_WORK_AVRCP_ATTRS | NY_BT_WORK_AVRCP_STATUS;
  nxmutex_unlock(&g_bt.lock);
}

static void
ny_bt_ct_passthrough_rsp(struct bt_avrcp_ct *ct, uint8_t tid,
                         bt_avrcp_rsp_t result,
                         const struct bt_avrcp_passthrough_rsp *rsp)
{
  (void)ct;
  (void)tid;
  if (rsp == NULL || result != BT_AVRCP_RSP_ACCEPTED ||
      (rsp->opid_state & 0x80) != 0)
    {
      return;
    }

  /* A key that went down has to come up again. */

  nxmutex_lock(&g_bt.lock);
  g_bt.release_opid = rsp->opid_state & 0x7f;
  g_bt.work |= NY_BT_WORK_AVRCP_RELEASE;
  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: ny_bt_ct_event
 *
 * Description:
 *   One player event, interim or changed.  A changed event also ends the
 *   registration, so it is asked for again.
 *
 ****************************************************************************/

static void ny_bt_ct_event(uint8_t event,
                           const struct bt_avrcp_event_data *data)
{
  if (data == NULL)
    {
      return;
    }

  nxmutex_lock(&g_bt.lock);
  if (event == BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED)
    {
      g_bt.play_status = data->play_status;
      g_bt.play_status_known = true;
    }
  else if (event == BT_AVRCP_EVT_TRACK_CHANGED)
    {
      g_bt.work |= NY_BT_WORK_AVRCP_ATTRS;
    }

  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_ct_notification(struct bt_avrcp_ct *ct, uint8_t tid,
                                  uint8_t status, uint8_t event,
                                  struct bt_avrcp_event_data *data)
{
  (void)ct;
  (void)tid;
  if (status == BT_AVRCP_STATUS_SUCCESS)
    {
      ny_bt_ct_event(event, data);
    }
}

static void ny_bt_ct_changed(struct bt_avrcp_ct *ct, uint8_t event,
                             struct bt_avrcp_event_data *data)
{
  (void)ct;
  ny_bt_ct_event(event, data);
  if (event == BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED)
    {
      ny_bt_work(NY_BT_WORK_AVRCP_PLAYBACK);
    }
  else if (event == BT_AVRCP_EVT_TRACK_CHANGED)
    {
      ny_bt_work(NY_BT_WORK_AVRCP_TRACK);
    }
}

static void ny_bt_ct_element_attrs(struct bt_avrcp_ct *ct, uint8_t tid,
                                   uint8_t status, struct net_buf *buf)
{
  char title[NY_BT_TEXT_MAX] = "";
  char artist[NY_BT_TEXT_MAX] = "";
  const uint8_t *cursor;
  size_t left;
  uint8_t count;

  (void)ct;
  (void)tid;
  if (status != BT_AVRCP_STATUS_SUCCESS || buf == NULL || buf->len < 1)
    {
      return;
    }

  /* Count, then per attribute: id (4), character set (2), length (2),
   * value; all big endian.
   */

  cursor = buf->data;
  left = buf->len;
  count = cursor[0];
  cursor++;
  left--;
  while (count-- > 0 && left >= 8)
    {
      uint32_t id = (uint32_t)cursor[0] << 24 | (uint32_t)cursor[1] << 16 |
                    (uint32_t)cursor[2] << 8 | cursor[3];
      uint16_t charset = (uint16_t)(cursor[4] << 8 | cursor[5]);
      size_t length = (size_t)cursor[6] << 8 | cursor[7];

      cursor += 8;
      left -= 8;
      if (length > left)
        {
          break;
        }

      if (charset == BT_AVRCP_CHARSET_UTF8)
        {
          if (id == BT_AVRCP_MEDIA_ATTR_TITLE)
            {
              ny_bt_text(title, sizeof(title), cursor, length);
            }
          else if (id == BT_AVRCP_MEDIA_ATTR_ARTIST)
            {
              ny_bt_text(artist, sizeof(artist), cursor, length);
            }
        }

      cursor += length;
      left -= length;
    }

  nxmutex_lock(&g_bt.lock);
  memcpy(g_bt.title, title, sizeof(g_bt.title));
  memcpy(g_bt.artist, artist, sizeof(g_bt.artist));
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_ct_play_status(struct bt_avrcp_ct *ct, uint8_t tid,
                                 uint8_t status, struct net_buf *buf)
{
  (void)ct;
  (void)tid;
  if (status != BT_AVRCP_STATUS_SUCCESS || buf == NULL ||
      buf->len < sizeof(struct bt_avrcp_get_play_status_rsp))
    {
      return;
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.play_status =
      ((const struct bt_avrcp_get_play_status_rsp *)buf->data)->play_status;
  g_bt.play_status_known = true;
  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: AVRCP target callbacks (the phone controls our volume)
 ****************************************************************************/

static void ny_bt_tg_connected(struct bt_conn *conn, struct bt_avrcp_tg *tg)
{
  (void)conn;
  nxmutex_lock(&g_bt.lock);
  g_bt.tg = tg;
  g_bt.volume_registered = false;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_tg_disconnected(struct bt_avrcp_tg *tg)
{
  nxmutex_lock(&g_bt.lock);
  if (g_bt.tg == tg)
    {
      g_bt.tg = NULL;
    }

  g_bt.volume_registered = false;
  g_bt.volume_absolute = -1;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_tg_unit_info(struct bt_avrcp_tg *tg, uint8_t tid)
{
  struct bt_avrcp_unit_info_rsp rsp = {
    .unit_type = BT_AVRCP_SUBUNIT_TYPE_PANEL,
    .company_id = NY_BT_SIG_COMPANY_ID,
  };

  bt_avrcp_tg_send_unit_info_rsp(tg, tid, &rsp);
}

static void ny_bt_tg_subunit_info(struct bt_avrcp_tg *tg, uint8_t tid)
{
  bt_avrcp_tg_send_subunit_info_rsp(tg, tid);
}

static void ny_bt_tg_get_caps(struct bt_avrcp_tg *tg, uint8_t tid,
                              uint8_t cap_id)
{
  struct net_buf *buf;

  if (cap_id != BT_AVRCP_CAP_EVENTS_SUPPORTED &&
      cap_id != BT_AVRCP_CAP_COMPANY_ID)
    {
      bt_avrcp_tg_get_caps(tg, tid, BT_AVRCP_STATUS_INVALID_PARAMETER, NULL);
      return;
    }

  /* No pool of our own: a pool the host's hand written pool table does not
   * list would be returned to the wrong place.  NULL takes the host's ACL
   * transmit pool.
   */

  buf = bt_avrcp_create_vendor_pdu(NULL);
  if (buf == NULL)
    {
      bt_avrcp_tg_get_caps(tg, tid, BT_AVRCP_STATUS_INTERNAL_ERROR, NULL);
      return;
    }

  net_buf_add_u8(buf, cap_id);
  net_buf_add_u8(buf, 1);
  if (cap_id == BT_AVRCP_CAP_EVENTS_SUPPORTED)
    {
      net_buf_add_u8(buf, BT_AVRCP_EVT_VOLUME_CHANGED);
    }
  else
    {
      net_buf_add_be24(buf, NY_BT_SIG_COMPANY_ID);
    }

  if (bt_avrcp_tg_get_caps(tg, tid, BT_AVRCP_STATUS_SUCCESS, buf) < 0)
    {
      net_buf_unref(buf);
    }
}

static void ny_bt_tg_register(struct bt_avrcp_tg *tg, uint8_t tid,
                              uint8_t event, uint32_t interval)
{
  struct bt_avrcp_event_data data;
#ifdef CONFIG_NYABULA_CORE_AUDIO
  struct ny_product_audio_levels_s levels;
#endif
  int absolute = BT_AVRCP_MAX_ABSOLUTE_VOLUME / 2;

  (void)interval;
  memset(&data, 0, sizeof(data));
  if (event != BT_AVRCP_EVT_VOLUME_CHANGED)
    {
      bt_avrcp_tg_notification(tg, tid, BT_AVRCP_STATUS_INVALID_PARAMETER,
                               event, &data);
      return;
    }

    /* The interim answer carries the volume as it is now.  Reading the level
     * is a copy out of the audio service, not a codec access.
     */

#ifdef CONFIG_NYABULA_CORE_AUDIO
  if (ny_product_audio_levels(&levels) == 0)
    {
      absolute = ny_bt_volume_to_absolute(levels.muted ? 0 : levels.volume);
    }
#endif

  data.absolute_volume = (uint8_t)absolute;
  if (bt_avrcp_tg_notification(tg, tid, BT_AVRCP_STATUS_SUCCESS, event,
                               &data) == 0)
    {
      nxmutex_lock(&g_bt.lock);
      g_bt.volume_registered = true;
      g_bt.volume_tid = tid;
      g_bt.volume_absolute = absolute;
      nxmutex_unlock(&g_bt.lock);
    }
}

static void ny_bt_tg_passthrough(struct bt_avrcp_tg *tg, uint8_t tid,
                                 struct net_buf *buf)
{
  bt_avrcp_rsp_t result = BT_AVRCP_RSP_NOT_IMPLEMENTED;
  struct net_buf *rsp;
  uint8_t opid_state = 0;

  if (buf != NULL && buf->len >= 1)
    {
      uint8_t opid;

      opid_state = buf->data[0];
      opid = opid_state & 0x7f;
      if (opid == BT_AVRCP_OPID_VOLUME_UP || opid == BT_AVRCP_OPID_VOLUME_DOWN)
        {
          /* A phone without absolute volume presses our volume keys. */

          result = BT_AVRCP_RSP_ACCEPTED;
          if ((opid_state & 0x80) == 0)
            {
              nxmutex_lock(&g_bt.lock);
              if (g_bt.volume_seen >= 0)
                {
                  int volume = g_bt.volume_pending >= 0 ? g_bt.volume_pending
                                                        : g_bt.volume_seen;

                  volume += opid == BT_AVRCP_OPID_VOLUME_UP
                                ? NY_BT_VOLUME_STEP
                                : -NY_BT_VOLUME_STEP;
                  g_bt.volume_pending = volume < 0     ? 0
                                        : volume > 100 ? 100
                                                       : volume;
                }

              nxmutex_unlock(&g_bt.lock);
            }
        }
    }

  rsp = bt_avrcp_create_pdu(NULL);
  if (rsp == NULL)
    {
      return;
    }

  net_buf_add_u8(rsp, opid_state);
  net_buf_add_u8(rsp, 0);
  if (bt_avrcp_tg_send_passthrough_rsp(tg, tid, result, rsp) < 0)
    {
      net_buf_unref(rsp);
    }
}

static void ny_bt_tg_volume(struct bt_avrcp_tg *tg, uint8_t tid,
                            uint8_t absolute)
{
  absolute &= BT_AVRCP_MAX_ABSOLUTE_VOLUME;

  /* Answer now; the codec is written by the tick. */

  nxmutex_lock(&g_bt.lock);
  g_bt.volume_absolute = absolute;
  g_bt.volume_pending = ny_bt_absolute_to_volume(absolute);
  nxmutex_unlock(&g_bt.lock);
  bt_avrcp_tg_absolute_volume(tg, tid, BT_AVRCP_STATUS_SUCCESS, absolute);
}

/****************************************************************************
 * Name: ny_bt_passthrough
 ****************************************************************************/

static int ny_bt_passthrough(uint8_t opid)
{
  struct bt_avrcp_ct *ct;

  nxmutex_lock(&g_bt.lock);
  ct = g_bt.ct;
  nxmutex_unlock(&g_bt.lock);
  if (ct == NULL)
    {
      return -ENOTCONN;
    }

  return bt_avrcp_ct_passthrough(ct, ny_bt_tid(), opid,
                                 BT_AVRCP_BUTTON_PRESSED, NULL, 0);
}

/****************************************************************************
 * Name: ny_bt_call_set
 ****************************************************************************/

static void ny_bt_call_set(struct bt_hfp_hf_call *call,
                           enum ny_bt_call_e state)
{
  nxmutex_lock(&g_bt.lock);
  if (state == NY_BT_CALL_IDLE)
    {
      if (g_bt.call == call || call == NULL)
        {
          g_bt.call = NULL;
          g_bt.call_state = NY_BT_CALL_IDLE;
          g_bt.number[0] = '\0';
        }
    }
  else
    {
      if (g_bt.call_state == NY_BT_CALL_IDLE)
        {
          g_bt.call_serial++;
        }

      g_bt.call = call;
      g_bt.call_state = state;
      if (state == NY_BT_CALL_INCOMING)
        {
          g_bt.work |= NY_BT_WORK_NOTIFY_CALL;
        }
    }

  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: call %s\n", g_bt_call_names[state]);
}

/****************************************************************************
 * Name: HFP hands-free callbacks
 ****************************************************************************/

static void ny_bt_hf_connected(struct bt_conn *conn, struct bt_hfp_hf *hf)
{
  (void)conn;
  nxmutex_lock(&g_bt.lock);
  g_bt.hf = hf;
  g_bt.codec_id = BT_HFP_HF_CODEC_CVSD;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: hfp service level connection up\n");
}

static void ny_bt_hf_disconnected(struct bt_hfp_hf *hf)
{
  ny_bt_audio_call_stop();
  nxmutex_lock(&g_bt.lock);
  if (g_bt.hf == hf)
    {
      g_bt.hf = NULL;
    }

  g_bt.call = NULL;
  g_bt.call_state = NY_BT_CALL_IDLE;
  g_bt.number[0] = '\0';
  g_bt.sco_up = false;
  g_bt.hfp_service = -1;
  g_bt.hfp_signal = -1;
  g_bt.hfp_battery = -1;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: hfp disconnected\n");
}

static int ny_bt_sco_send(const uint8_t *data, size_t size)
{
  struct bt_hfp_hf *hf;

  nxmutex_lock(&g_bt.lock);
  hf = g_bt.sco_up ? g_bt.hf : NULL;
  nxmutex_unlock(&g_bt.lock);
  return hf == NULL ? -ENOTCONN : Z_API(bt_hfp_hf_sco_send)(hf, data, size);
}

static void ny_bt_hf_sco_connected(struct bt_hfp_hf *hf, struct bt_conn *sco)
{
  struct bt_conn_info info;
  bool msbc;

  (void)hf;
  nxmutex_lock(&g_bt.lock);
  msbc = g_bt.codec_id == BT_HFP_HF_CODEC_MSBC;
  nxmutex_unlock(&g_bt.lock);

  /* The link itself knows better than the negotiation that preceded it. */

  if (sco != NULL && bt_conn_get_info(sco, &info) == 0 &&
      info.type == BT_CONN_TYPE_SCO)
    {
      if (info.sco.air_mode == BT_HCI_CODING_FORMAT_TRANSPARENT)
        {
          msbc = true;
        }
      else if (info.sco.air_mode == BT_HCI_CODING_FORMAT_CVSD)
        {
          msbc = false;
        }
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.sco_up = true;
  g_bt.codec_id = msbc ? BT_HFP_HF_CODEC_MSBC : BT_HFP_HF_CODEC_CVSD;
  g_bt.work |= NY_BT_WORK_QUIET_FLASH;
  nxmutex_unlock(&g_bt.lock);
  ny_bt_audio_call_start(msbc, ny_bt_sco_send);
  syslog(LOG_INFO, "nybt: call audio up, %s\n", msbc ? "mSBC" : "CVSD");
}

static void ny_bt_hf_sco_recv(struct bt_hfp_hf *hf, const uint8_t *data,
                              size_t size, uint8_t packet_status)
{
  (void)hf;
  ny_bt_audio_call_packet(data, size, packet_status);
}

static void ny_bt_hf_sco_disconnected(struct bt_conn *sco, uint8_t reason)
{
  (void)sco;
  ny_bt_audio_call_stop();
  nxmutex_lock(&g_bt.lock);
  g_bt.sco_up = false;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: call audio down, reason 0x%02x\n", reason);
}

static void ny_bt_hf_service(struct bt_hfp_hf *hf, uint32_t value)
{
  (void)hf;
  nxmutex_lock(&g_bt.lock);
  g_bt.hfp_service = (int)value;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_outgoing(struct bt_hfp_hf *hf,
                              struct bt_hfp_hf_call *call)
{
  (void)hf;
  ny_bt_call_set(call, NY_BT_CALL_DIALING);
}

static void ny_bt_hf_remote_ringing(struct bt_hfp_hf_call *call)
{
  ny_bt_call_set(call, NY_BT_CALL_ALERTING);
}

static void ny_bt_hf_incoming(struct bt_hfp_hf *hf,
                              struct bt_hfp_hf_call *call)
{
  (void)hf;
  ny_bt_call_set(call, NY_BT_CALL_INCOMING);
}

static void ny_bt_hf_accept(struct bt_hfp_hf_call *call)
{
  ny_bt_call_set(call, NY_BT_CALL_ACTIVE);
}

static void ny_bt_hf_ended(struct bt_hfp_hf_call *call)
{
  ny_bt_call_set(call, NY_BT_CALL_IDLE);
}

static void ny_bt_hf_held(struct bt_hfp_hf_call *call)
{
  ny_bt_call_set(call, NY_BT_CALL_HELD);
}

static void ny_bt_hf_signal(struct bt_hfp_hf *hf, uint32_t value)
{
  (void)hf;
  nxmutex_lock(&g_bt.lock);
  g_bt.hfp_signal = (int)value;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_battery(struct bt_hfp_hf *hf, uint32_t value)
{
  (void)hf;
  nxmutex_lock(&g_bt.lock);
  g_bt.hfp_battery = (int)value;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_ring(struct bt_hfp_hf_call *call)
{
  (void)call;

  /* A phone that does not send its ring tone down the audio link leaves
   * the ringing to us.
   */

  nxmutex_lock(&g_bt.lock);
  if (!g_bt.inband_ring && !g_bt.sco_up)
    {
      g_bt.work |= NY_BT_WORK_RING;
    }

  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_clip(struct bt_hfp_hf_call *call, char *number,
                          uint8_t type)
{
  (void)call;
  (void)type;
  if (number == NULL)
    {
      return;
    }

  nxmutex_lock(&g_bt.lock);
  if (g_bt.number[0] == '\0')
    {
      ny_bt_text(g_bt.number, sizeof(g_bt.number), (const uint8_t *)number,
                 strlen(number));
      if (g_bt.call_state == NY_BT_CALL_INCOMING)
        {
          g_bt.work |= NY_BT_WORK_NOTIFY_CALL;
        }
    }

  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_vgs(struct bt_hfp_hf *hf, uint8_t gain)
{
  (void)hf;
  if (gain > NY_BT_HFP_GAIN_MAX)
    {
      gain = NY_BT_HFP_GAIN_MAX;
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.volume_pending = gain * 100 / NY_BT_HFP_GAIN_MAX;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_inband(struct bt_hfp_hf *hf, bool inband)
{
  (void)hf;
  nxmutex_lock(&g_bt.lock);
  g_bt.inband_ring = inband;
  nxmutex_unlock(&g_bt.lock);
}

static void ny_bt_hf_codec(struct bt_hfp_hf *hf, uint8_t id)
{
  /* mSBC when the phone offers it, CVSD otherwise; both are handled. */

  nxmutex_lock(&g_bt.lock);
  g_bt.codec_id = id;
  nxmutex_unlock(&g_bt.lock);
  Z_API(bt_hfp_hf_select_codec)(hf, id);
}

/****************************************************************************
 * Name: ny_bt_apply_name
 *
 * Description:
 *   Carry the product's name: the one the panel, the access point and mDNS
 *   show.  Runs again whenever the owner renames the device.
 *
 ****************************************************************************/

static void ny_bt_apply_name(void)
{
  char name[33] = "Nyabula";

#ifdef CONFIG_NYABULA_CORE_NETWORK
  ny_product_network_name(name, sizeof(name));
#endif
  nxmutex_lock(&g_bt.lock);
  if (strcmp(name, g_bt.name) == 0)
    {
      nxmutex_unlock(&g_bt.lock);
      return;
    }

  snprintf(g_bt.name, sizeof(g_bt.name), "%s", name);
  nxmutex_unlock(&g_bt.lock);
#ifdef CONFIG_BT_DEVICE_NAME_DYNAMIC
  bt_set_name(name);
#endif
  bt_br_write_local_name(name);

  /* The inquiry response carries the name too; the host builds it from the
   * name set above.
   */

  bt_br_write_ext_inq_response(0);
}

/****************************************************************************
 * Name: ny_bt_bringup
 *
 * Description:
 *   Enabling the host talks to the controller and can take seconds, so it
 *   has a thread of its own.  It first waits for the controller's HCI node:
 *   a host enabled without one asserts.
 *
 ****************************************************************************/

static void *ny_bt_bringup(void *arg)
{
  uint64_t deadline = ny_product_time_ms(true) + NY_BT_CONTROLLER_WAIT_MS;
  bt_addr_le_t identity;
  size_t count = 1;
  int ret;

  (void)arg;
  while (access(CONFIG_BT_UART_ON_DEV_NAME, F_OK) < 0)
    {
      bool stop;

      nxmutex_lock(&g_bt.lock);
      stop = g_bt.stop;
      nxmutex_unlock(&g_bt.lock);
      if (stop || ny_product_time_ms(true) >= deadline)
        {
          ret = -ENODEV;
          goto failed;
        }

      usleep(500000);
    }

  ret = bt_enable(NULL);
  if (ret < 0 && ret != -EALREADY)
    {
      goto failed;
    }

#ifdef CONFIG_BT_SETTINGS
    /* Link keys: without this every restart forgets every phone.  The file
     * back-end creates its file but not the directory it is in.
     */

#ifdef CONFIG_SETTINGS_FILE_PATH
  {
    char directory[64];
    char *slash;

    snprintf(directory, sizeof(directory), "%s", CONFIG_SETTINGS_FILE_PATH);
    slash = strrchr(directory, '/');
    if (slash != NULL && slash != directory)
      {
        *slash = '\0';
        mkdir(directory, 0755);
      }
  }
#endif

  ret = settings_load();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "nybt: stored bonds not loaded: %d\n", ret);
    }
#endif

  bt_conn_cb_register(&g_bt_conn_cb);
  ret = bt_conn_auth_cb_register(&g_bt_auth_cb);
  if (ret < 0 && ret != -EALREADY)
    {
      goto failed;
    }

  bt_conn_auth_info_cb_register(&g_bt_auth_info_cb);

  /* The host has set SDP up by now (it does so before its own profiles),
   * which is what a record needs.
   */

  ret = bt_sdp_register_service(&g_bt_a2dp_sink_record);
  if (ret == 0)
    {
      ret = bt_a2dp_register_cb(&g_bt_a2dp_cb);
    }

  if (ret == 0)
    {
      ret = bt_a2dp_register_ep(&g_bt_sbc_endpoint, BT_AVDTP_AUDIO,
                                BT_AVDTP_SINK);
    }

  if (ret == 0)
    {
      ret = bt_avrcp_ct_register_cb(&g_bt_ct_cb);
    }

  if (ret == 0)
    {
      ret = bt_avrcp_tg_register_cb(&g_bt_tg_cb);
    }

  if (ret == 0)
    {
      ret = Z_API(bt_hfp_hf_register)(&g_bt_hf_cb);
    }

  if (ret < 0)
    {
      goto failed;
    }

  bt_id_get(&identity, &count);
  if (count > 0)
    {
      nxmutex_lock(&g_bt.lock);
      bt_addr_to_str(&identity.a, g_bt.address, sizeof(g_bt.address));
      nxmutex_unlock(&g_bt.lock);
    }

  bt_br_set_class_of_device(CONFIG_NYABULA_CORE_BT_COD);
  ny_bt_apply_name();

  /* Known phones may come back at any time; strangers only find us while
   * the owner has opened the pairing window.
   */

  ret = bt_br_set_connectable(true);
  if (ret < 0 && ret != -EALREADY)
    {
      goto failed;
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.state = NY_BT_READY;
  g_bt.error = 0;
  g_bt.autoconnect_at = ny_product_time_ms(true) + NY_BT_AUTOCONNECT_MS;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: ready\n");
  return NULL;

failed:
  nxmutex_lock(&g_bt.lock);
  g_bt.state = NY_BT_FAILED;
  g_bt.error = ret;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_ERR, "nybt: bring-up failed: %d\n", ret);
  return NULL;
}

/****************************************************************************
 * Name: ny_bt_devices_load / ny_bt_devices_save
 *
 * Description:
 *   The names and last-seen times of the phones.  The keys themselves are
 *   the host's to keep (CONFIG_BT_SETTINGS).  Product worker only.
 *
 ****************************************************************************/

static void ny_bt_devices_load(void)
{
  cJSON *root = NULL;
  uint64_t revision;
  const cJSON *row;
  int ret = ny_product_store_read(NY_BT_DOMAIN, &root, &revision);

  if (ret < 0)
    {
      nxmutex_lock(&g_bt.lock);
      g_bt.devices_retry = ny_product_time_ms(true) + 5000;
      nxmutex_unlock(&g_bt.lock);
      return;
    }

  nxmutex_lock(&g_bt.lock);
  cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(root, "devices"))
  {
    const cJSON *address = cJSON_GetObjectItemCaseSensitive(row, "address");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, "name");
    const cJSON *last = cJSON_GetObjectItemCaseSensitive(row, "lastConnected");
    struct ny_bt_device_s *device;
    bt_addr_t parsed;

    if (!cJSON_IsString(address) ||
        bt_addr_from_str(address->valuestring, &parsed) < 0)
      {
        continue;
      }

    device = ny_bt_device(address->valuestring, true);
    if (cJSON_IsString(name))
      {
        snprintf(device->name, sizeof(device->name), "%s", name->valuestring);
      }

    if (cJSON_IsNumber(last) && last->valuedouble > 0)
      {
        device->last_connected = (uint64_t)last->valuedouble;
      }
  }

  g_bt.devices_loaded = true;
  g_bt.devices_dirty = false;
  nxmutex_unlock(&g_bt.lock);
  cJSON_Delete(root);
}

static void ny_bt_devices_save(void)
{
  cJSON *previous = NULL;
  cJSON *root = cJSON_CreateObject();
  cJSON *rows = root ? cJSON_AddArrayToObject(root, "devices") : NULL;
  uint64_t revision = 0;
  bool valid = rows != NULL;
  int index;
  int ret;

  valid = valid && cJSON_AddNumberToObject(root, "schema", NY_BT_SCHEMA);
  nxmutex_lock(&g_bt.lock);
  for (index = 0; valid && index < NY_BT_DEVICES_MAX; index++)
    {
      const struct ny_bt_device_s *device = &g_bt.devices[index];
      cJSON *row;

      if (device->address[0] == '\0')
        {
          continue;
        }

      row = cJSON_CreateObject();
      valid = row != NULL &&
              cJSON_AddStringToObject(row, "address", device->address) &&
              cJSON_AddStringToObject(row, "name", device->name) &&
              cJSON_AddNumberToObject(row, "lastConnected",
                                      (double)device->last_connected);
      if (row != NULL && !cJSON_AddItemToArray(rows, row))
        {
          cJSON_Delete(row);
          valid = false;
        }
    }

  g_bt.devices_dirty = false;
  nxmutex_unlock(&g_bt.lock);
  ret = valid ? ny_product_store_read(NY_BT_DOMAIN, &previous, &revision)
              : -ENOMEM;
  cJSON_Delete(previous);
  if (ret == 0)
    {
      ret = ny_product_store_write(NY_BT_DOMAIN, root, revision, &revision);
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      nxmutex_lock(&g_bt.lock);
      g_bt.devices_dirty = true;
      g_bt.devices_retry = ny_product_time_ms(true) + 5000;
      nxmutex_unlock(&g_bt.lock);
    }
}

/****************************************************************************
 * Name: ny_bt_bond_mark
 ****************************************************************************/

static void ny_bt_bond_mark(const struct bt_bond_info *info, void *arg)
{
  char address[BT_ADDR_STR_LEN];
  struct ny_bt_device_s *device;

  (void)arg;
  bt_addr_to_str(&info->addr.a, address, sizeof(address));
  nxmutex_lock(&g_bt.lock);
  device = ny_bt_device(address, true);
  device->bonded = true;
  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: ny_bt_discoverable
 ****************************************************************************/

static int ny_bt_discoverable(bool enable, int seconds)
{
  int ret = bt_br_set_discoverable(enable);

  if (ret < 0 && ret != -EALREADY)
    {
      return ret;
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.discoverable = enable;
  g_bt.discoverable_until =
      enable ? ny_product_time_ms(true) + (uint64_t)seconds * 1000 : 0;
  nxmutex_unlock(&g_bt.lock);
  syslog(LOG_INFO, "nybt: pairing window %s\n", enable ? "open" : "closed");
  return 0;
}

/****************************************************************************
 * Name: ny_bt_tick_avrcp
 *
 * Description:
 *   One AVRCP command per tick, in the order a fresh connection needs them.
 *
 ****************************************************************************/

static void ny_bt_tick_avrcp(uint32_t work)
{
  struct bt_avrcp_ct *ct;
  uint8_t opid;
  uint32_t done;
  int ret = 0;

  nxmutex_lock(&g_bt.lock);
  ct = g_bt.ct;
  opid = g_bt.release_opid;
  nxmutex_unlock(&g_bt.lock);
  if (ct == NULL)
    {
      nxmutex_lock(&g_bt.lock);
      g_bt.work &= ~NY_BT_WORK_AVRCP_ANY;
      nxmutex_unlock(&g_bt.lock);
      return;
    }

  if (work & NY_BT_WORK_AVRCP_RELEASE)
    {
      done = NY_BT_WORK_AVRCP_RELEASE;
      ret = bt_avrcp_ct_passthrough(ct, ny_bt_tid(), opid,
                                    BT_AVRCP_BUTTON_RELEASED, NULL, 0);
    }
  else if (work & NY_BT_WORK_AVRCP_PAUSE)
    {
      done = NY_BT_WORK_AVRCP_PAUSE;
      ret = bt_avrcp_ct_passthrough(ct, ny_bt_tid(), BT_AVRCP_OPID_PAUSE,
                                    BT_AVRCP_BUTTON_PRESSED, NULL, 0);
    }
  else if (work & NY_BT_WORK_AVRCP_CAPS)
    {
      done = NY_BT_WORK_AVRCP_CAPS;
      ret =
          bt_avrcp_ct_get_caps(ct, ny_bt_tid(), BT_AVRCP_CAP_EVENTS_SUPPORTED);
    }
  else if (work & NY_BT_WORK_AVRCP_PLAYBACK)
    {
      done = NY_BT_WORK_AVRCP_PLAYBACK;
      ret = bt_avrcp_ct_register_notification(
          ct, ny_bt_tid(), BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED, 0,
          ny_bt_ct_changed);
    }
  else if (work & NY_BT_WORK_AVRCP_TRACK)
    {
      done = NY_BT_WORK_AVRCP_TRACK;
      ret = bt_avrcp_ct_register_notification(
          ct, ny_bt_tid(), BT_AVRCP_EVT_TRACK_CHANGED, 0, ny_bt_ct_changed);
    }
  else if (work & NY_BT_WORK_AVRCP_ATTRS)
    {
      struct net_buf *buf = bt_avrcp_create_vendor_pdu(NULL);

      done = NY_BT_WORK_AVRCP_ATTRS;
      if (buf != NULL)
        {
          /* Identifier 0 is "what is playing"; then the two wanted ids. */

          net_buf_add_be32(buf, 0);
          net_buf_add_be32(buf, 0);
          net_buf_add_u8(buf, 2);
          net_buf_add_be32(buf, BT_AVRCP_MEDIA_ATTR_TITLE);
          net_buf_add_be32(buf, BT_AVRCP_MEDIA_ATTR_ARTIST);
          ret = bt_avrcp_ct_get_element_attrs(ct, ny_bt_tid(), buf);
          if (ret < 0)
            {
              net_buf_unref(buf);
            }
        }
    }
  else
    {
      done = NY_BT_WORK_AVRCP_STATUS;
      ret = bt_avrcp_ct_get_play_status(ct, ny_bt_tid());
    }

  /* A command that did not go out is not repeated: a phone that lacks it
   * would be asked ten times a second for ever.  The next event asks again.
   */

  if (ret < 0)
    {
      syslog(LOG_DEBUG, "nybt: avrcp command 0x%x: %d\n", (unsigned)done, ret);
    }

  nxmutex_lock(&g_bt.lock);
  g_bt.work &= ~done;
  nxmutex_unlock(&g_bt.lock);
}

/****************************************************************************
 * Name: ny_bt_tick_volume
 *
 * Description:
 *   Keep the phone's volume slider and the product's volume on one number.
 *   The audio service owns the level; this only carries it both ways.
 *
 ****************************************************************************/

static void ny_bt_tick_volume(void)
{
#ifdef CONFIG_NYABULA_CORE_AUDIO
  struct ny_product_audio_levels_s levels;
  struct bt_avrcp_tg *tg = NULL;
  struct bt_hfp_hf *hf = NULL;
  uint8_t tid = 0;
  int pending;
  int volume;
  int absolute;

  nxmutex_lock(&g_bt.lock);
  pending = g_bt.volume_pending;
  g_bt.volume_pending = -1;
  nxmutex_unlock(&g_bt.lock);
  if (pending >= 0 && ny_product_audio_set_levels(
                          pending, pending > 0 ? 0 : NY_PRODUCT_AUDIO_KEEP,
                          NY_PRODUCT_AUDIO_KEEP) == 0)
    {
      /* What came from the phone is not news to the phone. */

      nxmutex_lock(&g_bt.lock);
      g_bt.volume_seen = pending;
      nxmutex_unlock(&g_bt.lock);
      return;
    }

  if (ny_product_audio_levels(&levels) < 0)
    {
      return;
    }

  volume = levels.muted ? 0 : levels.volume;
  absolute = ny_bt_volume_to_absolute(volume);
  nxmutex_lock(&g_bt.lock);
  if (volume != g_bt.volume_seen)
    {
      bool first = g_bt.volume_seen < 0;

      g_bt.volume_seen = volume;
      if (!first)
        {
          /* Compare on the phone's scale, and against what the phone's
           * last value maps back to: rounding alone is not a change.
           */

          if (g_bt.volume_registered && g_bt.tg != NULL &&
              absolute != g_bt.volume_absolute &&
              (g_bt.volume_absolute < 0 ||
               ny_bt_absolute_to_volume(g_bt.volume_absolute) != volume))
            {
              tg = g_bt.tg;
              tid = g_bt.volume_tid;
              g_bt.volume_registered = false;
              g_bt.volume_absolute = absolute;
            }

          if (g_bt.sco_up)
            {
              hf = g_bt.hf;
            }
        }
    }

  nxmutex_unlock(&g_bt.lock);
  if (tg != NULL)
    {
      struct bt_avrcp_event_data data;

      memset(&data, 0, sizeof(data));
      data.absolute_volume = (uint8_t)absolute;
      bt_avrcp_tg_notification(tg, tid, BT_AVRCP_STATUS_SUCCESS,
                               BT_AVRCP_EVT_VOLUME_CHANGED, &data);
    }

  if (hf != NULL)
    {
      Z_API(bt_hfp_hf_vgs)(hf, (uint8_t)(volume * NY_BT_HFP_GAIN_MAX / 100));
    }
#endif
}

/****************************************************************************
 * Name: ny_bt_status
 ****************************************************************************/

static cJSON *ny_bt_status(void)
{
  struct ny_bt_audio_status_s audio;
  cJSON *root = cJSON_CreateObject();
  cJSON *a2dp;
  cJSON *avrcp;
  cJSON *hfp;
  cJSON *sound;
  uint64_t now = ny_product_time_ms(true);
  bool ok;

  if (root == NULL)
    {
      return NULL;
    }

  ny_bt_audio_status(&audio);
  a2dp = cJSON_AddObjectToObject(root, "a2dp");
  avrcp = cJSON_AddObjectToObject(root, "avrcp");
  hfp = cJSON_AddObjectToObject(root, "hfp");
  sound = cJSON_AddObjectToObject(root, "audio");
  ok = a2dp != NULL && avrcp != NULL && hfp != NULL && sound != NULL;
  nxmutex_lock(&g_bt.lock);
  ok = ok &&
       cJSON_AddStringToObject(root, "state", g_bt_state_names[g_bt.state]);
  ok = ok && cJSON_AddNumberToObject(root, "error", g_bt.error);
  ok = ok && cJSON_AddStringToObject(root, "name", g_bt.name);
  ok = ok && cJSON_AddStringToObject(root, "address", g_bt.address);
  ok = ok &&
       cJSON_AddBoolToObject(root, "connectable", g_bt.state == NY_BT_READY);
  ok = ok && cJSON_AddBoolToObject(root, "discoverable", g_bt.discoverable);
  ok = ok && cJSON_AddNumberToObject(
                 root, "discoverableSeconds",
                 g_bt.discoverable && g_bt.discoverable_until > now
                     ? (double)((g_bt.discoverable_until - now + 999) / 1000)
                     : 0);
#ifdef CONFIG_BT_SETTINGS
  ok = ok && cJSON_AddBoolToObject(root, "bondsPersistent", true);
#else
  ok = ok && cJSON_AddBoolToObject(root, "bondsPersistent", false);
#endif
  if (g_bt.pairing != NULL)
    {
      cJSON *pairing = cJSON_AddObjectToObject(root, "pairing");
      struct ny_bt_device_s *device =
          ny_bt_device(g_bt.pairing_address, false);

      ok = ok && pairing != NULL &&
           cJSON_AddStringToObject(pairing, "address", g_bt.pairing_address) &&
           cJSON_AddStringToObject(pairing, "name",
                                   device != NULL ? device->name : "") &&
           cJSON_AddNumberToObject(
               pairing, "secondsLeft",
               g_bt.pairing_deadline > now
                   ? (double)((g_bt.pairing_deadline - now + 999) / 1000)
                   : 0);
    }

  if (g_bt.acl != NULL)
    {
      cJSON *peer = cJSON_AddObjectToObject(root, "connected");
      struct ny_bt_device_s *device = ny_bt_device(g_bt.peer_address, false);

      ok = ok && peer != NULL &&
           cJSON_AddStringToObject(peer, "address", g_bt.peer_address) &&
           cJSON_AddStringToObject(peer, "name",
                                   device != NULL ? device->name : "");
    }

  ok = ok && cJSON_AddStringToObject(a2dp, "state",
                                     g_bt_a2dp_names[g_bt.a2dp_state]);
  ok = ok && cJSON_AddStringToObject(a2dp, "codec",
                                     g_bt.a2dp_rate != 0 ? "SBC" : "none");
  ok = ok && cJSON_AddNumberToObject(a2dp, "sampleRate", g_bt.a2dp_rate);
  ok = ok && cJSON_AddNumberToObject(a2dp, "channels", g_bt.a2dp_channels);

  ok = ok && cJSON_AddBoolToObject(avrcp, "connected", g_bt.ct != NULL);
  ok = ok && cJSON_AddBoolToObject(
                 avrcp, "playing",
                 g_bt.play_status_known
                     ? g_bt.play_status == BT_AVRCP_PLAYBACK_STATUS_PLAYING
                     : g_bt.a2dp_state == NY_BT_A2DP_STREAMING);
  if (g_bt.title[0] != '\0')
    {
      ok = ok && cJSON_AddStringToObject(avrcp, "title", g_bt.title);
    }

  if (g_bt.artist[0] != '\0')
    {
      ok = ok && cJSON_AddStringToObject(avrcp, "artist", g_bt.artist);
    }

  ok = ok && cJSON_AddNumberToObject(
                 avrcp, "volume", g_bt.volume_seen < 0 ? 0 : g_bt.volume_seen);
  ok = ok && cJSON_AddBoolToObject(avrcp, "absoluteVolume",
                                   g_bt.volume_absolute >= 0);

  ok = ok && cJSON_AddStringToObject(
                 hfp, "state", g_bt.hf != NULL ? "connected" : "disconnected");
  ok = ok &&
       cJSON_AddStringToObject(hfp, "call", g_bt_call_names[g_bt.call_state]);
  if (g_bt.number[0] != '\0')
    {
      ok = ok && cJSON_AddStringToObject(hfp, "number", g_bt.number);
    }

  ok = ok && cJSON_AddBoolToObject(hfp, "inbandRing", g_bt.inband_ring);
  ok = ok && cJSON_AddBoolToObject(hfp, "audio", g_bt.sco_up);
  ok = ok && cJSON_AddStringToObject(hfp, "codec",
                                     !g_bt.sco_up ? "none"
                                     : g_bt.codec_id == BT_HFP_HF_CODEC_MSBC
                                         ? "mSBC"
                                         : "CVSD");
  if (g_bt.hfp_service >= 0)
    {
      ok = ok && cJSON_AddBoolToObject(hfp, "service", g_bt.hfp_service > 0);
    }

  if (g_bt.hfp_signal >= 0)
    {
      ok = ok && cJSON_AddNumberToObject(hfp, "signal", g_bt.hfp_signal);
    }

  if (g_bt.hfp_battery >= 0)
    {
      ok = ok && cJSON_AddNumberToObject(hfp, "battery", g_bt.hfp_battery);
    }

  nxmutex_unlock(&g_bt.lock);

  /* Who has the speaker, and the counters that tell a radio problem from a
   * codec problem from a scheduling problem.
   */

  ok = ok && cJSON_AddStringToObject(
                 sound, "owner",
                 audio.owner == NY_BT_AUDIO_OWNER_CALL    ? "call"
                 : audio.owner == NY_BT_AUDIO_OWNER_MEDIA ? "bluetooth"
                                                          : "none");
  ok = ok && cJSON_AddBoolToObject(sound, "held", audio.held);
  ok = ok && cJSON_AddNumberToObject(sound, "sampleRate", audio.rate);
  ok = ok && cJSON_AddNumberToObject(sound, "channels", audio.channels);
  ok = ok && cJSON_AddNumberToObject(sound, "error", audio.error);
  ok = ok && cJSON_AddNumberToObject(sound, "bufferMs", audio.media_buffer_ms);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "mediaPackets", audio.media_packets);
  ok = ok && cJSON_AddNumberToObject(sound, "mediaFrames", audio.media_frames);
  ok = ok && cJSON_AddNumberToObject(sound, "mediaBadFrames", audio.media_bad);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "mediaDropped", audio.media_dropped);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "mediaUnderruns", audio.media_underruns);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "scoRxPackets", audio.sco_rx_packets);
  ok = ok && cJSON_AddNumberToObject(sound, "scoRxBad", audio.sco_rx_bad);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "scoTxPackets", audio.sco_tx_packets);
  ok =
      ok && cJSON_AddNumberToObject(sound, "scoTxFailed", audio.sco_tx_failed);
  ok = ok && cJSON_AddBoolToObject(sound, "micGated", audio.gate_closed);
  ok = ok &&
       cJSON_AddNumberToObject(sound, "micGateClosures", audio.gate_closures);
  if (!ok)
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

/****************************************************************************
 * Name: ny_bt_devices
 ****************************************************************************/

static int ny_bt_devices(cJSON **result)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *rows = root ? cJSON_AddArrayToObject(root, "devices") : NULL;
  bool ok = rows != NULL;
  int index;

  if (ok)
    {
      nxmutex_lock(&g_bt.lock);
      for (index = 0; index < NY_BT_DEVICES_MAX; index++)
        {
          g_bt.devices[index].bonded = false;
        }

      nxmutex_unlock(&g_bt.lock);
      if (bt_is_ready())
        {
          bt_foreach_bond_br(ny_bt_bond_mark, NULL);
        }
    }

  nxmutex_lock(&g_bt.lock);
  for (index = 0; ok && index < NY_BT_DEVICES_MAX; index++)
    {
      const struct ny_bt_device_s *device = &g_bt.devices[index];
      cJSON *row;

      if (device->address[0] == '\0')
        {
          continue;
        }

      row = cJSON_CreateObject();
      ok = row != NULL &&
           cJSON_AddStringToObject(row, "address", device->address) &&
           cJSON_AddStringToObject(row, "name", device->name) &&
           cJSON_AddBoolToObject(row, "bonded", device->bonded) &&
           cJSON_AddBoolToObject(
               row, "connected",
               g_bt.acl != NULL &&
                   strcasecmp(device->address, g_bt.peer_address) == 0) &&
           cJSON_AddNumberToObject(row, "lastConnected",
                                   (double)device->last_connected);
      if (row != NULL && !cJSON_AddItemToArray(rows, row))
        {
          cJSON_Delete(row);
          ok = false;
        }
    }

  nxmutex_unlock(&g_bt.lock);
  if (!ok)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  *result = root;
  return 0;
}

/****************************************************************************
 * Name: ny_bt_target
 *
 * Description:
 *   The address a request is about: the one it names, else the connected
 *   phone, else the phone that was connected last.
 *
 ****************************************************************************/

static int ny_bt_target(const cJSON *data, bool required, bt_addr_t *address,
                        char *text)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, "address");
  const struct ny_bt_device_s *latest = NULL;
  int index;

  text[0] = '\0';
  if (item != NULL)
    {
      if (!cJSON_IsString(item) ||
          strlen(item->valuestring) != BT_ADDR_STR_LEN - 1 ||
          bt_addr_from_str(item->valuestring, address) < 0)
        {
          return -EINVAL;
        }

      bt_addr_to_str(address, text, BT_ADDR_STR_LEN);
      return 0;
    }

  if (required)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_bt.lock);
  if (g_bt.peer_address[0] != '\0')
    {
      snprintf(text, BT_ADDR_STR_LEN, "%s", g_bt.peer_address);
    }
  else
    {
      for (index = 0; index < NY_BT_DEVICES_MAX; index++)
        {
          const struct ny_bt_device_s *device = &g_bt.devices[index];

          if (device->address[0] != '\0' && device->last_connected > 0 &&
              (latest == NULL ||
               device->last_connected > latest->last_connected))
            {
              latest = device;
            }
        }

      if (latest != NULL)
        {
          snprintf(text, BT_ADDR_STR_LEN, "%s", latest->address);
        }
    }

  nxmutex_unlock(&g_bt.lock);
  return text[0] == '\0' || bt_addr_from_str(text, address) < 0 ? -ENOENT : 0;
}

#endif /* CONFIG_NYABULA_CORE_BT */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_NYABULA_CORE_BT
/****************************************************************************
 * Name: btsnoop_log_capture
 *
 * Description:
 *   ZBlue's H4 driver hands every HCI packet to the btsnoop writer of the
 *   openvela Bluetooth framework, unconditionally.  This product drives
 *   ZBlue directly and does not build that framework; until this service
 *   existed nothing referenced the host stack, the linker dropped the H4
 *   driver with it, and the missing symbol never showed.  Weak, so that a
 *   configuration that does build the framework keeps its real writer.
 *
 ****************************************************************************/

void weak_function btsnoop_log_capture(uint8_t is_receive, uint8_t *hci_pkt,
                                       uint32_t hci_pkt_size)
{
  (void)is_receive;
  (void)hci_pkt;
  (void)hci_pkt_size;
}
#endif

/****************************************************************************
 * Name: ny_product_bt_request
 ****************************************************************************/

int ny_product_bt_request(const struct ny_product_caller_s *caller,
                          const char *topic, const cJSON *data, cJSON **result)
{
  if (strncmp(topic, "bt.", 3) != 0)
    {
      return -ENOSYS;
    }

#ifndef CONFIG_NYABULA_CORE_BT
  (void)caller;
  (void)data;
  (void)result;
  return -ENODEV;
#else
  bool status = strcmp(topic, "bt.status") == 0;
  struct bt_hfp_hf_call *call;
  enum ny_bt_call_e call_state;
  enum ny_bt_state_e state;
  char text[BT_ADDR_STR_LEN];
  bt_addr_t address;
  int ret = 0;

  if (caller->role != NY_PRODUCT_OWNER &&
      !(status && caller->role == NY_PRODUCT_FAMILY))
    {
      return -EACCES;
    }

  nxmutex_lock(&g_bt.lock);
  state = g_bt.state;
  call = g_bt.call;
  call_state = g_bt.call_state;
  nxmutex_unlock(&g_bt.lock);
  if (status)
    {
      *result = ny_bt_status();
      return *result == NULL ? -ENOMEM : 0;
    }

  if (strcmp(topic, "bt.devices") == 0)
    {
      return ny_bt_devices(result);
    }

  if (state != NY_BT_READY)
    {
      return state == NY_BT_FAILED ? -ENODEV : -EAGAIN;
    }

  if (strcmp(topic, "bt.discoverable") == 0)
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
      const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(data, "seconds");
      int window = CONFIG_NYABULA_CORE_BT_DISCOVERABLE_SECONDS;

      if (!cJSON_IsBool(enabled) ||
          (seconds != NULL &&
           (!cJSON_IsNumber(seconds) || !isfinite(seconds->valuedouble) ||
            seconds->valuedouble < 1 ||
            seconds->valuedouble > NY_BT_DISCOVERABLE_MAX ||
            floor(seconds->valuedouble) != seconds->valuedouble)))
        {
          return -EINVAL;
        }

      if (seconds != NULL)
        {
          window = seconds->valueint;
        }

      ret = ny_bt_discoverable(cJSON_IsTrue(enabled), window);
    }
  else if (strcmp(topic, "bt.pair.confirm") == 0)
    {
      const cJSON *accept = cJSON_GetObjectItemCaseSensitive(data, "accept");
      struct bt_conn *pairing = NULL;
      bool passkey_style;

      if (!cJSON_IsBool(accept))
        {
          return -EINVAL;
        }

      ret = ny_bt_target(data, false, &address, text);
      nxmutex_lock(&g_bt.lock);
      if (g_bt.pairing != NULL &&
          (cJSON_GetObjectItemCaseSensitive(data, "address") == NULL ||
           (ret == 0 && strcasecmp(text, g_bt.pairing_address) == 0)))
        {
          pairing = g_bt.pairing;
          g_bt.pairing = NULL;
        }

      passkey_style = g_bt.pairing_passkey_style;
      nxmutex_unlock(&g_bt.lock);
      if (pairing == NULL)
        {
          return -ENOENT;
        }

      ret = !cJSON_IsTrue(accept) ? bt_conn_auth_cancel(pairing)
            : passkey_style       ? bt_conn_auth_passkey_confirm(pairing)
                                  : bt_conn_auth_pairing_confirm(pairing);
      bt_conn_unref(pairing);
    }
  else if (strcmp(topic, "bt.forget") == 0)
    {
      struct ny_bt_device_s *device;
      struct bt_conn *conn;

      ret = ny_bt_target(data, true, &address, text);
      if (ret < 0)
        {
          return ret;
        }

      conn = bt_conn_lookup_addr_br(&address);
      if (conn != NULL)
        {
          bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
          bt_conn_unref(conn);
        }

      ret = bt_br_unpair(&address);
      nxmutex_lock(&g_bt.lock);
      device = ny_bt_device(text, false);
      if (device != NULL)
        {
          memset(device, 0, sizeof(*device));
          g_bt.devices_dirty = true;
          ret = 0;
        }

      nxmutex_unlock(&g_bt.lock);
    }
  else if (strcmp(topic, "bt.connect") == 0)
    {
      struct bt_conn *conn;

      ret = ny_bt_target(data, false, &address, text);
      if (ret < 0)
        {
          return ret;
        }

      nxmutex_lock(&g_bt.lock);
      ret =
          g_bt.acl != NULL
              ? (strcasecmp(text, g_bt.peer_address) == 0 ? -EALREADY : -EBUSY)
              : 0;
      nxmutex_unlock(&g_bt.lock);
      if (ret < 0)
        {
          return ret == -EALREADY ? 0 : ret;
        }

      conn = bt_conn_create_br(&address, BT_BR_CONN_PARAM_DEFAULT);
      if (conn == NULL)
        {
          return -EIO;
        }

      /* The reference from the create call becomes the one g_bt.acl
       * holds; the connected callback then finds it in place.
       */

      nxmutex_lock(&g_bt.lock);
      if (g_bt.acl == NULL)
        {
          g_bt.acl = conn;
          g_bt.acl_initiated = true;
          g_bt.acl_attempt_until =
              ny_product_time_ms(true) + NY_BT_PAGE_GIVE_UP_MS;
          conn = NULL;
        }

      nxmutex_unlock(&g_bt.lock);
      if (conn != NULL)
        {
          bt_conn_unref(conn);
        }
    }
  else if (strcmp(topic, "bt.disconnect") == 0)
    {
      struct bt_conn *conn;

      ret = ny_bt_target(data, false, &address, text);
      if (ret < 0)
        {
          return ret;
        }

      conn = bt_conn_lookup_addr_br(&address);
      if (conn == NULL)
        {
          return -ENOTCONN;
        }

      ret = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
      bt_conn_unref(conn);
    }
  else if (strcmp(topic, "bt.media.play") == 0)
    {
      ret = ny_bt_passthrough(BT_AVRCP_OPID_PLAY);
    }
  else if (strcmp(topic, "bt.media.pause") == 0)
    {
      ret = ny_bt_passthrough(BT_AVRCP_OPID_PAUSE);
    }
  else if (strcmp(topic, "bt.media.stop") == 0)
    {
      ret = ny_bt_passthrough(BT_AVRCP_OPID_STOP);
    }
  else if (strcmp(topic, "bt.media.next") == 0)
    {
      ret = ny_bt_passthrough(BT_AVRCP_OPID_FORWARD);
    }
  else if (strcmp(topic, "bt.media.prev") == 0)
    {
      ret = ny_bt_passthrough(BT_AVRCP_OPID_BACKWARD);
    }
  else if (strcmp(topic, "bt.call.answer") == 0)
    {
      ret = call == NULL || call_state != NY_BT_CALL_INCOMING
                ? -ENOENT
                : Z_API(bt_hfp_hf_accept)(call);
    }
  else if (strcmp(topic, "bt.call.reject") == 0)
    {
      ret = call == NULL || call_state != NY_BT_CALL_INCOMING
                ? -ENOENT
                : Z_API(bt_hfp_hf_reject)(call);
    }
  else if (strcmp(topic, "bt.call.hangup") == 0)
    {
      ret = call == NULL ? -ENOENT
            : call_state == NY_BT_CALL_INCOMING
                ? Z_API(bt_hfp_hf_reject)(call)
                : Z_API(bt_hfp_hf_terminate)(call);
    }
  else
    {
      return -ENOSYS;
    }

  if (ret < 0)
    {
      return ret;
    }

  *result = ny_bt_status();
  return *result == NULL ? -ENOMEM : 0;
#endif
}

#ifdef CONFIG_NYABULA_CORE_BT

/****************************************************************************
 * Name: ny_product_bt_tick
 ****************************************************************************/

int ny_product_bt_tick(void)
{
  uint64_t now = ny_product_time_ms(true);
  struct bt_conn *pairing = NULL;
  struct bt_conn *stale = NULL;
  struct bt_conn *acl = NULL;
  char pairing_address[BT_ADDR_STR_LEN];
  char number[NY_BT_NUMBER_MAX];
  enum ny_bt_state_e state;
  bool start = false;
  bool close_window = false;
  bool autoconnect = false;
  bool load;
  bool save;
  uint32_t call_serial;
  uint32_t work;

  nxmutex_lock(&g_bt.lock);
  state = g_bt.state;
  if (state == NY_BT_OFF && !g_bt.stop)
    {
      g_bt.state = NY_BT_STARTING;
      start = true;
    }

  load = !g_bt.devices_loaded && now >= g_bt.devices_retry;
  nxmutex_unlock(&g_bt.lock);
  if (start)
    {
      pthread_attr_t attr;
      int ret = ny_bt_audio_start();

      if (ret == 0)
        {
          pthread_attr_init(&attr);
          pthread_attr_setstacksize(&attr, CONFIG_NYABULA_CORE_BT_STACKSIZE);
          ret = -pthread_create(&g_bt.thread, &attr, ny_bt_bringup, NULL);
          pthread_attr_destroy(&attr);
        }

      nxmutex_lock(&g_bt.lock);
      if (ret < 0)
        {
          g_bt.state = NY_BT_FAILED;
          g_bt.error = ret;
        }
      else
        {
          g_bt.thread_started = true;
        }

      nxmutex_unlock(&g_bt.lock);
      return ret;
    }

  if (load)
    {
      ny_bt_devices_load();
    }

  if (state != NY_BT_READY)
    {
      return 0;
    }

  nxmutex_lock(&g_bt.lock);
  work = g_bt.work;
  g_bt.work &= NY_BT_WORK_AVRCP_ANY | NY_BT_WORK_AVRCP_CONNECT;
  if (g_bt.discoverable && now >= g_bt.discoverable_until)
    {
      close_window = true;
    }

  if (g_bt.pairing != NULL && now >= g_bt.pairing_deadline)
    {
      pairing = g_bt.pairing;
      g_bt.pairing = NULL;
    }

  if (g_bt.acl != NULL && g_bt.acl_attempt_until != 0 &&
      now >= g_bt.acl_attempt_until)
    {
      /* The phone never answered and the host never said so: without this
       * the slot would stay taken and every later bt.connect be refused.
       */

      stale = g_bt.acl;
      g_bt.acl = NULL;
      g_bt.acl_initiated = false;
      g_bt.acl_attempt_until = 0;
    }

  if (work & (NY_BT_WORK_A2DP_CONNECT | NY_BT_WORK_NAME_REQUEST))
    {
      acl = g_bt.acl != NULL ? bt_conn_ref(g_bt.acl) : NULL;
    }

  if ((work & NY_BT_WORK_AVRCP_CONNECT) != 0 && g_bt.acl != NULL &&
      g_bt.ct == NULL && now >= g_bt.acl_since + NY_BT_AVRCP_CONNECT_MS)
    {
      g_bt.work &= ~NY_BT_WORK_AVRCP_CONNECT;
      if (acl == NULL)
        {
          acl = bt_conn_ref(g_bt.acl);
        }
    }
  else
    {
      work &= ~NY_BT_WORK_AVRCP_CONNECT;
    }

  if (g_bt.autoconnect_at != 0 && now >= g_bt.autoconnect_at)
    {
      g_bt.autoconnect_at = 0;
      autoconnect = g_bt.acl == NULL && g_bt.devices_loaded;
    }

  memcpy(pairing_address, g_bt.pairing_address, sizeof(pairing_address));
  memcpy(number, g_bt.number, sizeof(number));
  call_serial = g_bt.call_serial;
  save =
      g_bt.devices_dirty && g_bt.devices_loaded && now >= g_bt.devices_retry;
  nxmutex_unlock(&g_bt.lock);

  if (close_window)
    {
      ny_bt_discoverable(false, 0);
    }

  if (stale != NULL)
    {
      bt_conn_disconnect(stale, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
      bt_conn_unref(stale);
    }

  if (pairing != NULL)
    {
      /* Nobody answered on the panel: that is a no. */

      bt_conn_auth_cancel(pairing);
      bt_conn_unref(pairing);
    }

  if (work & NY_BT_WORK_QUIET_FLASH)
    {
      ny_product_media_sleep();
    }

  if (work & NY_BT_WORK_RING)
    {
      ny_product_media_alert(NY_PRODUCT_MEDIA_ALERT_ONCE);
    }

  if (work & NY_BT_WORK_NOTIFY_PAIRING)
    {
      char id[48];
      char title[96];

      snprintf(id, sizeof(id), "bt-pair-%s-%u", pairing_address,
               (unsigned)(now / 1000));
      snprintf(title, sizeof(title), "Bluetooth pairing request: %s",
               pairing_address);
      ny_product_notification_post(id, "bluetooth", title,
                                   ny_product_time_ms(false) +
                                       NY_BT_PAIRING_MS);
    }

  if (work & NY_BT_WORK_NOTIFY_CALL)
    {
      char id[48];
      char title[96];

      /* One notice per call: the id is the same when the number arrives
       * after the first ring, and the second post is then ignored.
       */

      snprintf(id, sizeof(id), "bt-call-%u-%d", (unsigned)call_serial,
               number[0] != '\0');
      snprintf(title, sizeof(title), "Incoming call%s%s",
               number[0] != '\0' ? ": " : "", number);
      ny_product_notification_post(id, "bluetooth", title,
                                   ny_product_time_ms(false) + 60000);
    }

  if (acl != NULL)
    {
      if (work & NY_BT_WORK_NAME_REQUEST)
        {
          const bt_addr_t *peer = bt_conn_get_dst_br(acl);

          if (peer != NULL)
            {
              bt_br_remote_name_request(peer, ny_bt_remote_name);
            }
        }

      if (work & NY_BT_WORK_A2DP_CONNECT)
        {
          /* We called the phone, so the first profile is ours to open; the
           * phone follows with the rest, hands-free included.
           */

          if (bt_a2dp_connect(acl) == NULL)
            {
              syslog(LOG_INFO, "nybt: a2dp connect not started\n");
            }
        }

      if (work & NY_BT_WORK_AVRCP_CONNECT)
        {
          bt_avrcp_connect(acl);
        }

      bt_conn_unref(acl);
    }

  if (autoconnect)
    {
#if defined(CONFIG_NYABULA_CORE_BT_AUTOCONNECT) && defined(CONFIG_BT_SETTINGS)
      char text[BT_ADDR_STR_LEN];
      bt_addr_t address;
      cJSON *empty = cJSON_CreateObject();

      if (empty != NULL && ny_bt_target(empty, false, &address, text) == 0)
        {
          struct bt_conn *conn =
              bt_conn_create_br(&address, BT_BR_CONN_PARAM_DEFAULT);

          nxmutex_lock(&g_bt.lock);
          if (conn != NULL && g_bt.acl == NULL)
            {
              g_bt.acl = conn;
              g_bt.acl_initiated = true;
              g_bt.acl_attempt_until = now + NY_BT_PAGE_GIVE_UP_MS;
              conn = NULL;
            }

          nxmutex_unlock(&g_bt.lock);
          if (conn != NULL)
            {
              bt_conn_unref(conn);
            }
        }

      cJSON_Delete(empty);
#endif
    }

  if (work & NY_BT_WORK_AVRCP_ANY)
    {
      ny_bt_tick_avrcp(work);
    }

  ny_bt_tick_volume();
  ny_bt_apply_name();
  if (save)
    {
      ny_bt_devices_save();
    }

  return 0;
}

/****************************************************************************
 * Name: ny_product_bt_shutdown
 *
 * Description:
 *   The product services stop: no sound and no strangers.  The host itself
 *   stays up; its threads and pools are not made to be torn down, and a
 *   product start finds it ready.
 *
 ****************************************************************************/

void ny_product_bt_shutdown(void)
{
  enum ny_bt_state_e state;

  nxmutex_lock(&g_bt.lock);
  state = g_bt.state;
  nxmutex_unlock(&g_bt.lock);
  if (state == NY_BT_READY)
    {
      ny_bt_discoverable(false, 0);
    }

  ny_bt_audio_call_stop();
  ny_bt_audio_media_stop();
}

/****************************************************************************
 * Name: ny_product_bt_speaker_claim
 ****************************************************************************/

int ny_product_bt_speaker_claim(bool alert)
{
  int ret = ny_bt_audio_hold(NY_BT_CLAIM_MS);

  if (ret == 0 && !alert)
    {
      /* Flash music takes over for good: have the phone stop sending.  For
       * a chime the phone is left playing and is heard again afterwards.
       */

      nxmutex_lock(&g_bt.lock);
      if (g_bt.a2dp_state == NY_BT_A2DP_STREAMING)
        {
          g_bt.work |= NY_BT_WORK_AVRCP_PAUSE;
        }

      nxmutex_unlock(&g_bt.lock);
    }

  return ret;
}

/****************************************************************************
 * Name: ny_product_bt_speaker_release
 ****************************************************************************/

void ny_product_bt_speaker_release(void) { ny_bt_audio_release(); }

/****************************************************************************
 * Name: ny_product_bt_call_active
 ****************************************************************************/

bool ny_product_bt_call_active(void)
{
  bool active;

  nxmutex_lock(&g_bt.lock);
  active = g_bt.sco_up || (g_bt.call_state != NY_BT_CALL_IDLE &&
                           g_bt.call_state != NY_BT_CALL_INCOMING);
  nxmutex_unlock(&g_bt.lock);
  return active;
}

/****************************************************************************
 * Name: ny_product_bt_speaker_source
 ****************************************************************************/

const char *ny_product_bt_speaker_source(void)
{
  struct ny_bt_audio_status_s audio;

  ny_bt_audio_status(&audio);
  return audio.owner == NY_BT_AUDIO_OWNER_CALL    ? "call"
         : audio.owner == NY_BT_AUDIO_OWNER_MEDIA ? "bluetooth"
                                                  : NULL;
}

#endif /* CONFIG_NYABULA_CORE_BT */
