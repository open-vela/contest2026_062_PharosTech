/****************************************************************************
 * app/nyabula_core/ny_product_network.c
 *
 * Wi-Fi provisioning state machine (P1 desk-pet flow):
 *
 *   IDLE ──provision.start──▶ AP_PROVISION ──wifi.set──▶ STA_CONNECTING
 *     ▲                          │  (SoftAP + DHCPD +      │ (wapi psk/essid
 *     │                          │   QR on screen)         │  + dhcpc)
 *     └───── provision.stop ─────┘                         ▼
 *                              STA_FAILED ◀──timeout── STA_ONLINE
 *                                  │ (retry / back to AP)
 *
 * Topics (all owner-only unless noted):
 *   network.status            any role, current state + interfaces + QR text
 *   network.provision.start   {ssid?, psk?, channel?} -> start SoftAP
 *   network.provision.stop    tear down SoftAP, back to IDLE
 *   network.wifi.scan         STA scan (list of {ssid, rssi, freq, encode})
 *   network.wifi.set          {ssid, psk} -> persist + connect as station
 *   network.wifi.forget       clear stored credentials
 *
 * Credentials persist in the product store domain "wifi":
 *   {"ssid": "...", "psk": "...", "auto": true}
 *
 * The board-tested command sequence (实测日志.md) is reproduced through the
 * WEXT API in apps/include/wireless/wapi.h:
 *   AP : wapi mode wlan0 3; wapi freq wlan0 <ch> 1; wapi psk wlan0 <pw> 3 3;
 *        wapi essid wlan0 <ssid> 1; ifconfig wlan0 192.168.4.1; dhcpd
 *   STA: wapi mode wlan0 2; wapi psk wlan0 <pw> 3 3; wapi essid wlan0 <ssid>
 *1; ifconfig wlan0 dhcp
 ****************************************************************************/

#include "ny_product.h"
#include "ny_product_store.h"
#include "ny_web.h"

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(CONFIG_NYABULA_CORE_EYE) && defined(CONFIG_NYABULA_CORE_WEB)
#include <nyabula_eye_engine.h>
#include <nyabula_eye_service.h>
#endif

#ifdef CONFIG_WIRELESS_WAPI
#include <wireless/wapi.h>
#endif
#ifdef CONFIG_NETUTILS_DHCPD
#include <netutils/dhcpd.h>
#endif
#ifdef CONFIG_NETUTILS_DHCPC
#include <netutils/dhcpc.h>
#endif
#ifdef CONFIG_NETUTILS_NETLIB
#include <netutils/netlib.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NYABULA_CORE_NETWORK_IFNAME
#define CONFIG_NYABULA_CORE_NETWORK_IFNAME "wlan0"
#endif

#define NY_NET_IFNAME        CONFIG_NYABULA_CORE_NETWORK_IFNAME
#define NY_NET_STORE_DOMAIN  "wifi"
#define NY_NET_AP_SSID_FMT   "Nyabula-%02X%02X"
#define NY_NET_AP_PSK        "nyabula123"
#define NY_NET_AP_CHANNEL    6
#define NY_NET_AP_ADDR       "192.168.4.1"
#define NY_NET_AP_MASK       "255.255.255.0"
#define NY_NET_AP_POOL_START "192.168.4.100"
#define NY_NET_STA_TIMEOUT_S 25
#define NY_NET_STA_RETRIES   3
#define NY_NET_STA_RETRY_S   5
#define NY_NET_LOAD_RETRIES  150 /* worker ticks, about 15 s */
#define NY_NET_SSID_MAX      32
#define NY_NET_PSK_MAX       64
#define NY_NET_SCAN_MAX      24

#define NY_NET_EYE_SOURCE    "network"
#define NY_NET_EYE_PRIORITY  70
#define NY_NET_EYE_LEASE_MS  15000
#define NY_NET_EYE_RENEW_S   5
#define NY_NET_EYE_ONLINE_MS 60000

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_net_state_e
{
  NY_NET_IDLE = 0,
  NY_NET_AP_PROVISION,
  NY_NET_STA_CONNECTING,
  NY_NET_STA_ONLINE,
  NY_NET_STA_FAILED,
};

struct ny_net_s
{
  enum ny_net_state_e state;
  char ap_ssid[NY_NET_SSID_MAX + 1];
  char ap_psk[NY_NET_PSK_MAX + 1];
  int ap_channel;
  char sta_ssid[NY_NET_SSID_MAX + 1];
  char sta_psk[NY_NET_PSK_MAX + 1];
  char sta_ipv4[INET_ADDRSTRLEN];
  time_t sta_started;
  time_t sta_retry_at; /* no station attempt before this */
  int sta_attempt;
  int last_error;
  bool pending_sta;  /* wifi.set arrived, worker must (re)connect */
  bool pending_ap;   /* provision.start arrived, worker must bring AP up */
  bool pending_stop; /* provision.stop arrived */
  uint64_t revision;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_net_lock = NXMUTEX_INITIALIZER;
static struct ny_net_s g_net;
static bool g_net_loaded;
static bool g_net_boot_decided;
static int g_net_load_attempts;

static const char *g_net_state_names[] = { "idle", "ap_provision",
                                           "sta_connecting", "sta_online",
                                           "sta_failed" };

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *ny_net_text(const cJSON *row, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool ny_net_copy(char *dst, size_t size, const char *src)
{
  size_t length = src ? strlen(src) : 0;
  if (length == 0 || length >= size)
    return false;
  memcpy(dst, src, length + 1);
  return true;
}

/* Load persisted STA credentials into g_net (lock held). */

static void ny_net_load(void)
{
  cJSON *value = NULL;
  if (g_net_loaded)
    return;

  /* The store reports "no such entry" as success with no value, so a
   * failure here means the store itself is not there -- at boot, usually
   * because the partition holding it has not been mounted yet.  That must
   * not be read as "this device has no network": ask again on later ticks,
   * and only after a bounded wait settle for what can be had.
   */

  int ret =
      ny_product_store_read(NY_NET_STORE_DOMAIN, &value, &g_net.revision);
  if (ret < 0 && g_net_load_attempts++ < NY_NET_LOAD_RETRIES)
    return;
  g_net_loaded = true;
  if (ret < 0 || value == NULL)
    return;
  ny_net_copy(g_net.sta_ssid, sizeof(g_net.sta_ssid),
              ny_net_text(value, "ssid"));
  ny_net_copy(g_net.sta_psk, sizeof(g_net.sta_psk), ny_net_text(value, "psk"));
  cJSON *automatic = cJSON_GetObjectItemCaseSensitive(value, "auto");
  if (g_net.sta_ssid[0] != '\0' &&
      (automatic == NULL || cJSON_IsTrue(automatic)))
    g_net.pending_sta = true; /* auto-connect on boot */
  cJSON_Delete(value);
}

static int ny_net_save(void)
{
  cJSON *value = cJSON_CreateObject();
  if (value == NULL)
    return -ENOMEM;
  cJSON_AddStringToObject(value, "ssid", g_net.sta_ssid);
  cJSON_AddStringToObject(value, "psk", g_net.sta_psk);
  cJSON_AddBoolToObject(value, "auto", true);
  int ret = ny_product_store_write(NY_NET_STORE_DOMAIN, value, g_net.revision,
                                   &g_net.revision);
  cJSON_Delete(value);
  return ret;
}

/* Derive a per-device AP SSID from the MAC address. */

static void ny_net_default_ap_ssid(char *out, size_t size)
{
  uint8_t mac[6] = { 0 };
#ifdef CONFIG_NETUTILS_NETLIB
  netlib_getmacaddr(NY_NET_IFNAME, mac);
#endif
  snprintf(out, size, NY_NET_AP_SSID_FMT, mac[4], mac[5]);
}

/* Request the provisioning AP, filling in the identity nobody supplied
 * (lock held).  Two callers reach this without a provision.start request
 * ever having run -- a device with no stored network, and a stored network
 * that stopped answering -- and both would otherwise bring up an access
 * point with an empty name.
 */

static void ny_net_arm_ap(void)
{
  if (g_net.ap_ssid[0] == '\0')
    ny_net_default_ap_ssid(g_net.ap_ssid, sizeof(g_net.ap_ssid));
  if (g_net.ap_psk[0] == '\0')
    ny_net_copy(g_net.ap_psk, sizeof(g_net.ap_psk), NY_NET_AP_PSK);
  if (g_net.ap_channel == 0)
    g_net.ap_channel = NY_NET_AP_CHANNEL;
  g_net.pending_ap = true;
}

static int __attribute__((unused)) ny_net_query_ipv4(char *out, size_t size)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct ifreq request = { 0 };
  if (fd < 0)
    return -errno;
  snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", NY_NET_IFNAME);
  int ret = ioctl(fd, SIOCGIFADDR, (unsigned long)(uintptr_t)&request);
  close(fd);
  if (ret < 0 || request.ifr_addr.sa_family != AF_INET)
    return -ENOTCONN;
  struct sockaddr_in *ipv4 = (struct sockaddr_in *)&request.ifr_addr;
  if (ipv4->sin_addr.s_addr == 0)
    return -ENOTCONN;
  inet_ntop(AF_INET, &ipv4->sin_addr, out, size);
  return 0;
}

/* --- Hardware actions (called WITHOUT the lock, from the worker tick) --- */

#ifdef CONFIG_WIRELESS_WAPI
static int ny_net_apply_security(int sock, const char *psk)
{
  /* Mirrors `wapi psk wlan0 <pw> 3 3`: CCMP + WPA2 */

  int ret = wpa_driver_wext_set_key_ext(sock, NY_NET_IFNAME, WPA_ALG_CCMP, psk,
                                        strlen(psk));
  if (ret < 0)
    return ret;
  ret = wpa_driver_wext_set_auth_param(
      sock, NY_NET_IFNAME, IW_AUTH_WPA_VERSION, IW_AUTH_WPA_VERSION_WPA2);
  if (ret < 0)
    return ret;
  ret = wpa_driver_wext_set_auth_param(
      sock, NY_NET_IFNAME, IW_AUTH_CIPHER_PAIRWISE, IW_AUTH_CIPHER_CCMP);
  if (ret < 0)
    return ret;
  return wpa_driver_wext_set_auth_param(sock, NY_NET_IFNAME, IW_AUTH_KEY_MGMT,
                                        IW_AUTH_KEY_MGMT_PSK);
}
#endif

static int ny_net_hw_ap_start(const char *ssid, const char *psk, int channel)
{
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  if (sock < 0)
    return sock;
  int ret = wapi_set_mode(sock, NY_NET_IFNAME, WAPI_MODE_MASTER);
  if (ret == 0)
    ret = wapi_set_freq(sock, NY_NET_IFNAME, (double)channel, WAPI_FREQ_FIXED);
  if (ret == 0 && psk[0] != '\0')
    ret = ny_net_apply_security(sock, psk);
  if (ret == 0)
    ret = wapi_set_essid(sock, NY_NET_IFNAME, ssid, WAPI_ESSID_ON);
  if (ret == 0)
    {
      struct in_addr addr;
      inet_pton(AF_INET, NY_NET_AP_ADDR, &addr);
      ret = wapi_set_ip(sock, NY_NET_IFNAME, &addr);
      inet_pton(AF_INET, NY_NET_AP_MASK, &addr);
      if (ret == 0)
        ret = wapi_set_netmask(sock, NY_NET_IFNAME, &addr);
    }
  close(sock);
#ifdef CONFIG_NETUTILS_NETLIB
  if (ret == 0)
    ret = netlib_ifup(NY_NET_IFNAME);
#endif
#ifdef CONFIG_NETUTILS_DHCPD
  if (ret == 0)
    {
      struct in_addr addr;

      /* dhcpd keeps its addresses in host order -- it counts leases by
       * adding to the start address -- while inet_pton produces network
       * order.  Passed through unconverted, the pool started at
       * 100.4.168.192 with a netmask of 0.255.255.255.
       */

      inet_pton(AF_INET, NY_NET_AP_POOL_START, &addr);
      dhcpd_set_startip(ntohl(addr.s_addr));
      inet_pton(AF_INET, NY_NET_AP_ADDR, &addr);
      dhcpd_set_routerip(ntohl(addr.s_addr));
      dhcpd_set_dnsip(ntohl(addr.s_addr));
      inet_pton(AF_INET, NY_NET_AP_MASK, &addr);
      dhcpd_set_netmask(ntohl(addr.s_addr));
      ret = dhcpd_start(NY_NET_IFNAME);
    }
#endif
  return ret;
#else
  (void)ssid;
  (void)psk;
  (void)channel;
  return -ENOSYS;
#endif
}

static int ny_net_hw_ap_stop(void)
{
#ifdef CONFIG_NETUTILS_DHCPD
  dhcpd_stop();
#endif
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  if (sock < 0)
    return sock;
  wpa_driver_wext_disconnect(sock, NY_NET_IFNAME);
  int ret = wapi_set_mode(sock, NY_NET_IFNAME, WAPI_MODE_MANAGED);
  close(sock);
  return ret;
#else
  return -ENOSYS;
#endif
}

static int ny_net_hw_sta_connect(const char *ssid, const char *psk)
{
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  if (sock < 0)
    return sock;
  wpa_driver_wext_disconnect(sock, NY_NET_IFNAME);
  int ret = wapi_set_mode(sock, NY_NET_IFNAME, WAPI_MODE_MANAGED);
  if (ret == 0 && psk[0] != '\0')
    ret = ny_net_apply_security(sock, psk);
  if (ret == 0)
    ret = wapi_set_essid(sock, NY_NET_IFNAME, ssid, WAPI_ESSID_ON);
  close(sock);
#ifdef CONFIG_NETUTILS_NETLIB
  if (ret == 0)
    ret = netlib_ifup(NY_NET_IFNAME);
#endif
  return ret;
#else
  (void)ssid;
  (void)psk;
  return -ENOSYS;
#endif
}

/* Blocking DHCP request; called from the worker after association. */

static int ny_net_hw_dhcp(char *ipv4, size_t size)
{
#if defined(CONFIG_NETUTILS_DHCPC) && defined(CONFIG_NETUTILS_NETLIB)
  uint8_t mac[6];
  struct dhcpc_state ds;
  netlib_getmacaddr(NY_NET_IFNAME, mac);
  void *handle = dhcpc_open(NY_NET_IFNAME, mac, sizeof(mac));
  if (handle == NULL)
    return -ENOMEM;
  int ret = dhcpc_request(handle, &ds);
  dhcpc_close(handle);
  if (ret < 0)
    return ret;
  netlib_set_ipv4addr(NY_NET_IFNAME, &ds.ipaddr);
  if (ds.netmask.s_addr != 0)
    netlib_set_ipv4netmask(NY_NET_IFNAME, &ds.netmask);
  if (ds.default_router.s_addr != 0)
    netlib_set_dripv4addr(NY_NET_IFNAME, &ds.default_router);
  if (ds.dnsaddr.s_addr != 0)
    netlib_set_ipv4dnsaddr(&ds.dnsaddr);
  inet_ntop(AF_INET, &ds.ipaddr, ipv4, size);
  return 0;
#else
  return ny_net_query_ipv4(ipv4, size);
#endif
}

static cJSON *ny_net_hw_scan(int *error)
{
  cJSON *list = cJSON_CreateArray();
  *error = 0;
  if (list == NULL)
    {
      *error = -ENOMEM;
      return NULL;
    }
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  if (sock < 0)
    {
      *error = sock;
      return list;
    }
  int ret = wapi_scan_init(sock, NY_NET_IFNAME, NULL);
  int waited = 0;
  while (ret == 0 && waited < 60)
    {
      ret = wapi_scan_stat(sock, NY_NET_IFNAME);
      if (ret <= 0)
        break;
      usleep(100000);
      waited++;
      ret = 0;
    }
  struct wapi_list_s aps = { 0 };
  if (ret == 0)
    ret = wapi_scan_coll(sock, NY_NET_IFNAME, &aps);
  if (ret == 0)
    {
      int count = 0;
      struct wapi_scan_info_s *info;
      for (info = aps.head.scan; info && count < NY_NET_SCAN_MAX;
           info = info->next)
        {
          if (!info->has_essid || info->essid[0] == '\0')
            continue;
          cJSON *item = cJSON_CreateObject();
          if (item == NULL)
            break;
          cJSON_AddStringToObject(item, "ssid", info->essid);
          if (info->has_rssi)
            cJSON_AddNumberToObject(item, "rssi", info->rssi);
          if (info->has_freq)
            cJSON_AddNumberToObject(item, "freq", info->freq);
          if (info->has_encode)
            cJSON_AddBoolToObject(item, "secure", info->encode != 0);
          cJSON_AddItemToArray(list, item);
          count++;
        }
      wapi_scan_coll_free(&aps);
    }
  *error = ret;
  close(sock);
#else
  *error = -ENOSYS;
#endif
  return list;
}

/* --- Result builders (lock held) --- */

static cJSON *ny_net_status_json(bool owner)
{
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return NULL;
  cJSON_AddStringToObject(root, "state", g_net_state_names[g_net.state]);
  cJSON_AddStringToObject(root, "ifname", NY_NET_IFNAME);
  cJSON_AddBoolToObject(root, "configured", g_net.sta_ssid[0] != '\0');
  cJSON_AddNumberToObject(root, "error", g_net.last_error);
  cJSON_AddNumberToObject(root, "attempt", g_net.sta_attempt);
  if (g_net.sta_ssid[0] != '\0')
    cJSON_AddStringToObject(root, "ssid", g_net.sta_ssid);
  if (g_net.sta_ipv4[0] != '\0')
    cJSON_AddStringToObject(root, "ipv4", g_net.sta_ipv4);
  if (g_net.state == NY_NET_AP_PROVISION)
    {
      char qr[160];
      cJSON *ap = cJSON_AddObjectToObject(root, "ap");
      if (ap != NULL)
        {
          cJSON_AddStringToObject(ap, "ssid", g_net.ap_ssid);
          if (owner)
            cJSON_AddStringToObject(ap, "psk", g_net.ap_psk);
          cJSON_AddNumberToObject(ap, "channel", g_net.ap_channel);
          cJSON_AddStringToObject(ap, "ipv4", NY_NET_AP_ADDR);
          snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", g_net.ap_ssid,
                   g_net.ap_psk);
          cJSON_AddStringToObject(ap, "qr_wifi", qr);
          snprintf(qr, sizeof(qr), "http://%s/#/provision", NY_NET_AP_ADDR);
          cJSON_AddStringToObject(ap, "qr_url", qr);
        }
    }
  return root;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_network_request
 ****************************************************************************/

int ny_product_network_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result)
{
  if (strncmp(topic, "network.", 8) != 0)
    return -ENOSYS;

  int ret = nxmutex_lock(&g_net_lock);
  if (ret < 0)
    return ret;
  ny_net_load();

  if (strcmp(topic, "network.status") == 0)
    {
      *result = ny_net_status_json(caller->role == NY_PRODUCT_OWNER);
      ret = *result ? 0 : -ENOMEM;
      goto out;
    }

  if (caller->role != NY_PRODUCT_OWNER)
    {
      ret = -EACCES;
      goto out;
    }

  if (strcmp(topic, "network.provision.start") == 0)
    {
      const char *ssid = ny_net_text(data, "ssid");
      const char *psk = ny_net_text(data, "psk");
      const cJSON *channel = cJSON_GetObjectItemCaseSensitive(data, "channel");
      if (ssid == NULL ||
          !ny_net_copy(g_net.ap_ssid, sizeof(g_net.ap_ssid), ssid))
        ny_net_default_ap_ssid(g_net.ap_ssid, sizeof(g_net.ap_ssid));
      if (psk == NULL || !ny_net_copy(g_net.ap_psk, sizeof(g_net.ap_psk), psk))
        ny_net_copy(g_net.ap_psk, sizeof(g_net.ap_psk), NY_NET_AP_PSK);
      g_net.ap_channel = cJSON_IsNumber(channel) ? (int)channel->valuedouble
                                                 : NY_NET_AP_CHANNEL;
      g_net.pending_ap = true;
      g_net.pending_stop = false;
      *result = ny_net_status_json(true);
      ret = *result ? 0 : -ENOMEM;
    }
  else if (strcmp(topic, "network.provision.stop") == 0)
    {
      g_net.pending_stop = true;
      g_net.pending_ap = false;
      *result = ny_net_status_json(true);
      ret = *result ? 0 : -ENOMEM;
    }
  else if (strcmp(topic, "network.wifi.set") == 0)
    {
      const char *ssid = ny_net_text(data, "ssid");
      const char *psk = ny_net_text(data, "psk");
      if (!ny_net_copy(g_net.sta_ssid, sizeof(g_net.sta_ssid), ssid))
        {
          ret = -EINVAL;
          goto out;
        }
      if (psk == NULL ||
          !ny_net_copy(g_net.sta_psk, sizeof(g_net.sta_psk), psk))
        g_net.sta_psk[0] = '\0';
      ret = ny_net_save();
      if (ret < 0)
        goto out;
      g_net.sta_attempt = 0;
      g_net.sta_retry_at = 0;
      g_net.pending_sta = true;
      *result = ny_net_status_json(true);
      ret = *result ? 0 : -ENOMEM;
    }
  else if (strcmp(topic, "network.wifi.forget") == 0)
    {
      g_net.sta_ssid[0] = '\0';
      g_net.sta_psk[0] = '\0';
      g_net.sta_ipv4[0] = '\0';
      g_net.pending_sta = false;
      ret = ny_net_save();
      if (ret == 0)
        {
          *result = ny_net_status_json(true);
          ret = *result ? 0 : -ENOMEM;
        }
    }
  else if (strcmp(topic, "network.wifi.scan") == 0)
    {
      /* Scanning blocks up to ~6 s; release the lock while it runs. */

      nxmutex_unlock(&g_net_lock);
      int error;
      cJSON *list = ny_net_hw_scan(&error);
      if (list == NULL)
        return -ENOMEM;
      cJSON *root = cJSON_CreateObject();
      if (root == NULL)
        {
          cJSON_Delete(list);
          return -ENOMEM;
        }
      cJSON_AddItemToObject(root, "networks", list);
      cJSON_AddNumberToObject(root, "error", error);
      *result = root;
      return 0;
    }
  else
    {
      ret = -ENOSYS;
    }

out:
  nxmutex_unlock(&g_net_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_net_step
 *
 *   Runs from the nyproduct worker (~10 Hz). Executes at most one hardware
 *   transition per call so the worker never stalls other services for
 *   longer than a single wapi/dhcp operation.
 ****************************************************************************/

static int ny_net_step(void)
{
  int ret = nxmutex_lock(&g_net_lock);
  if (ret < 0)
    return ret;
  ny_net_load();

  /* A device that has never been told about a network has exactly one
   * useful thing to do, which is to let someone tell it.  Decided once, as
   * soon as the store has answered: after that an idle radio is a choice
   * the owner made with provision.stop or wifi.forget, and is left alone.
   */

  if (g_net_loaded && !g_net_boot_decided)
    {
      g_net_boot_decided = true;
      if (g_net.sta_ssid[0] == '\0' && g_net.state == NY_NET_IDLE &&
          !g_net.pending_ap && !g_net.pending_sta)
        ny_net_arm_ap();
    }

  /* Snapshot the requested action, then act without holding the lock. */

  bool do_stop = g_net.pending_stop;
  bool do_ap = !do_stop && g_net.pending_ap;
  bool do_sta = !do_stop && !do_ap && g_net.pending_sta &&
                time(NULL) >= g_net.sta_retry_at;
  bool check_sta = g_net.state == NY_NET_STA_CONNECTING;
  char ssid[NY_NET_SSID_MAX + 1];
  char psk[NY_NET_PSK_MAX + 1];
  int channel = g_net.ap_channel;
  strcpy(ssid, do_ap ? g_net.ap_ssid : g_net.sta_ssid);
  strcpy(psk, do_ap ? g_net.ap_psk : g_net.sta_psk);
  g_net.pending_stop = false;
  g_net.pending_ap = false;
  if (do_sta)
    g_net.pending_sta = false;
  nxmutex_unlock(&g_net_lock);

  int error = 0;
  enum ny_net_state_e next = (enum ny_net_state_e) - 1;
  char ipv4[INET_ADDRSTRLEN] = { 0 };

  if (do_stop)
    {
      error = ny_net_hw_ap_stop();
      next = NY_NET_IDLE;
    }
  else if (do_ap)
    {
      error = ny_net_hw_ap_start(ssid, psk, channel);
      next = error == 0 ? NY_NET_AP_PROVISION : NY_NET_IDLE;
    }
  else if (do_sta)
    {
      if (g_net.state == NY_NET_AP_PROVISION)
        ny_net_hw_ap_stop();
      error = ny_net_hw_sta_connect(ssid, psk);
      next = error == 0 ? NY_NET_STA_CONNECTING : NY_NET_STA_FAILED;
    }
  else if (check_sta)
    {
      /* Association is asynchronous in the driver; once the interface has
       * come up, DHCP (blocking, bounded by dhcpc timeouts) gets an address.
       */

      error = ny_net_hw_dhcp(ipv4, sizeof(ipv4));
      if (error == 0)
        next = NY_NET_STA_ONLINE;
      else if (time(NULL) - g_net.sta_started > NY_NET_STA_TIMEOUT_S)
        next = NY_NET_STA_FAILED;
    }
  else
    {
      return 0;
    }

  ret = nxmutex_lock(&g_net_lock);
  if (ret < 0)
    return ret;
  /* An attempt is an attempt whichever way it ends.  The driver refuses a
   * network it cannot see at once, without ever reaching CONNECTING, and
   * counting only the attempts that got that far left such a failure at
   * zero for ever: retried on every tick, never reaching the fallback, and
   * the device unreachable because the AP had already been taken down.
   */

  if (do_sta)
    g_net.sta_attempt++;

  if (next != (enum ny_net_state_e) - 1)
    {
      if (next == NY_NET_STA_CONNECTING)
        {
          g_net.sta_started = time(NULL);
          g_net.sta_ipv4[0] = '\0';
        }
      else if (next == NY_NET_STA_ONLINE)
        {
          strcpy(g_net.sta_ipv4, ipv4);
        }
      else if (next == NY_NET_STA_FAILED)
        {
          /* Retry a few times, then fall back to provisioning so the user
           * can rescan the QR code on the desk pet.
           */

          if (g_net.sta_attempt < NY_NET_STA_RETRIES)
            {
              g_net.pending_sta = true;
              g_net.sta_retry_at = time(NULL) + NY_NET_STA_RETRY_S;
            }
          else
            {
              g_net.sta_attempt = 0;
              ny_net_arm_ap();
            }
        }
      g_net.state = next;
    }
  g_net.last_error = error;
  nxmutex_unlock(&g_net_lock);
  return 0;
}

#if defined(CONFIG_NYABULA_CORE_EYE) && defined(CONFIG_NYABULA_CORE_WEB)

/* Put one command to the Eye Engine under this module's own source, so its
 * scene competes with, and is released independently of, anyone else's.
 */

static int ny_net_eye_submit(const char *action, cJSON *params,
                             uint32_t lease_ms)
{
  cJSON *command = cJSON_CreateObject();
  char *json = NULL;
  int ret = -ENOMEM;
  if (command != NULL &&
      cJSON_AddStringToObject(command, "action", action) != NULL &&
      cJSON_AddStringToObject(command, "id", "network") != NULL &&
      cJSON_AddStringToObject(command, "source", NY_NET_EYE_SOURCE) != NULL &&
      cJSON_AddNumberToObject(command, "priority", NY_NET_EYE_PRIORITY) !=
          NULL &&
      cJSON_AddNumberToObject(command, "lease_ms", lease_ms) != NULL)
    {
      if (params != NULL)
        {
          cJSON_AddItemToObject(command, "params", params);
          params = NULL;
        }
      json = cJSON_PrintUnformatted(command);
    }
  if (json != NULL)
    {
      ret = nyabula_eye_service_submit(NY_NET_EYE_SOURCE, json, strlen(json));

      /* The pairing command carries the access token. */

      memset(json, 0, strlen(json));
      free(json);
    }
  cJSON_Delete(params);
  cJSON_Delete(command);
  return ret;
}

/* Build the payload for the pairing scene: the address of the setup page on
 * one eye, the access point's own join code on the other.  A phone scans
 * the second to get onto the device's network and the first to open the
 * page, and the page is given the token so that holding the device is all
 * the owner has to prove.
 */

/* The join-code format gives backslash, semicolon, comma, colon and the
 * double quote a meaning, so they are escaped where they occur in a name or
 * a passphrase.  The default identity has none; one chosen through
 * provision.start may.
 */

static void ny_net_qr_escape(char *out, size_t size, const char *in)
{
  size_t used = 0;
  for (; *in != '\0' && used + 2 < size; in++)
    {
      if (strchr("\\;,:\"", *in) != NULL)
        out[used++] = '\\';
      out[used++] = *in;
    }
  out[used] = '\0';
}

static cJSON *ny_net_eye_pairing(const char *ssid, const char *psk)
{
  char token[80];
  char text[NYABULA_EYE_TEXT_QR];
  char name[NY_NET_SSID_MAX * 2 + 2];
  char pass[NY_NET_PSK_MAX * 2 + 2];
  cJSON *payload = cJSON_CreateObject();
  if (payload == NULL)
    return NULL;
  ny_net_qr_escape(name, sizeof(name), ssid);
  ny_net_qr_escape(pass, sizeof(pass), psk);

  /* A code that does not fit is left out rather than cut short: the engine
   * refuses an over-long text, and a shortened one would be a wrong one.
   */

  if (snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", name, pass) <
      (int)sizeof(text))
    cJSON_AddStringToObject(payload, "qr_right", text);
  memset(pass, 0, sizeof(pass));
  if (ny_web_product_token(token, sizeof(token)) == 0)
    {
      snprintf(text, sizeof(text),
               "http://" NY_NET_AP_ADDR "/#/provision?token=%s", token);
      cJSON_AddStringToObject(payload, "qr_left", text);
      memset(token, 0, sizeof(token));
    }
  memset(text, 0, sizeof(text));
  return payload;
}

/* Keep the eyes saying what the network is doing.
 *
 * The pairing scene is shown on a short lease and renewed from here rather
 * than shown once for good: if this worker ever stops, the code disappears
 * by itself instead of being burnt into the face of a device that is no
 * longer listening.
 */

static void ny_net_eye_sync(void)
{
  static enum ny_net_state_e shown = NY_NET_IDLE;
  static time_t renew_at;
  char ssid[NY_NET_SSID_MAX + 1];
  char psk[NY_NET_PSK_MAX + 1];
  char ipv4[INET_ADDRSTRLEN];
  enum ny_net_state_e state;
  time_t now = time(NULL);
  cJSON *params;
  cJSON *payload;

  if (nxmutex_lock(&g_net_lock) < 0)
    return;
  state = g_net.state;
  strcpy(ssid, g_net.ap_ssid);
  strcpy(psk, g_net.ap_psk);
  strcpy(ipv4, g_net.sta_ipv4);
  nxmutex_unlock(&g_net_lock);

  if (state == NY_NET_AP_PROVISION)
    {
      bool renew = shown == NY_NET_AP_PROVISION;
      if (renew && now < renew_at)
        return;
      payload = ny_net_eye_pairing(ssid, psk);
      params = cJSON_CreateObject();
      if (payload == NULL || params == NULL)
        {
          cJSON_Delete(payload);
          cJSON_Delete(params);
          return;
        }
      if (!renew)
        {
          cJSON_AddStringToObject(params, "scene", "qr");
          cJSON_AddStringToObject(params, "style", "full");
        }
      cJSON_AddItemToObject(params, "payload", payload);

      /* A renewal that finds no scene to renew -- the engine was not up
       * yet, or the lease ran out under load -- falls back to showing it
       * again on the next pass.
       */

      if (ny_net_eye_submit(renew ? "eyes.scene.update" : "eyes.scene.show",
                            params, NY_NET_EYE_LEASE_MS) == 0)
        shown = NY_NET_AP_PROVISION;
      else
        shown = NY_NET_IDLE;
      renew_at = now + NY_NET_EYE_RENEW_S;
    }
  else if (state == NY_NET_STA_ONLINE)
    {
      if (shown == NY_NET_STA_ONLINE)
        return;

      /* Once, for long enough to read: this is how the owner learns the
       * address to open now that the access point is gone.
       */

      payload = cJSON_CreateObject();
      params = cJSON_CreateObject();
      if (payload == NULL || params == NULL)
        {
          cJSON_Delete(payload);
          cJSON_Delete(params);
          return;
        }
      /* The address alone is not enough.  The page keeps the token in the
       * browser's storage for the origin it was opened from, and that was
       * the access point's address; at the new one the phone knows nothing.
       * So the new address goes out the way the first one did, as a code
       * that carries the token, with the plain text beside it for anyone
       * typing it into a computer.
       */

      {
        char token[80];
        char text[NYABULA_EYE_TEXT_QR];
        if (ny_web_product_token(token, sizeof(token)) == 0 &&
            snprintf(text, sizeof(text), "http://%s/#/?token=%s", ipv4,
                     token) < (int)sizeof(text))
          cJSON_AddStringToObject(payload, "qr_left", text);
        else if (snprintf(text, sizeof(text), "http://%s/", ipv4) <
                 (int)sizeof(text))
          cJSON_AddStringToObject(payload, "qr_left", text);
        memset(token, 0, sizeof(token));
        memset(text, 0, sizeof(text));
      }

      cJSON_AddStringToObject(payload, "title", "网络已连接");
      cJSON_AddStringToObject(payload, "detail", ipv4);
      cJSON_AddStringToObject(params, "scene", "qr");
      cJSON_AddStringToObject(params, "style", "full");
      cJSON_AddItemToObject(params, "payload", payload);
      if (ny_net_eye_submit("eyes.scene.show", params, NY_NET_EYE_ONLINE_MS) ==
          0)
        shown = NY_NET_STA_ONLINE;
    }
  else if (shown != NY_NET_IDLE)
    {
      ny_net_eye_submit("eyes.scene.hide", NULL, NY_NET_EYE_LEASE_MS);
      shown = NY_NET_IDLE;
    }
}
#endif

/****************************************************************************
 * Name: ny_product_network_tick
 ****************************************************************************/

int ny_product_network_tick(void)
{
  int ret = ny_net_step();
#if defined(CONFIG_NYABULA_CORE_EYE) && defined(CONFIG_NYABULA_CORE_WEB)
  ny_net_eye_sync();
#endif
  return ret;
}

/****************************************************************************
 * Name: ny_product_network_shutdown
 ****************************************************************************/

void ny_product_network_shutdown(void)
{
  if (nxmutex_lock(&g_net_lock) < 0)
    return;
  bool ap = g_net.state == NY_NET_AP_PROVISION;
  g_net.state = NY_NET_IDLE;
  nxmutex_unlock(&g_net_lock);
  if (ap)
    ny_net_hw_ap_stop();
}
