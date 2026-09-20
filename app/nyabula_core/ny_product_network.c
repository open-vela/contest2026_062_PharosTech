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
 *   network.name.set          {"name": "..."}; "" goes back to the default
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
#include <limits.h>
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
#define NY_NET_RSSI_PERIOD_S 10
#define NY_NET_LOAD_RETRIES  150 /* worker ticks, about 15 s */
#define NY_NET_SSID_MAX      32
#define NY_NET_PSK_MAX       64
#define NY_NET_SCAN_MAX      24

#define NY_NET_EYE_SOURCE    "network"
#define NY_NET_EYE_PRIORITY  70
#define NY_NET_EYE_LEASE_MS  15000
#define NY_NET_EYE_RENEW_S   5
#define NY_NET_EYE_ONLINE_MS 60000

/* What each code is for, shown above it.  The two on the access point are
 * numbered because they have to be used in that order.  Any text added
 * here must also be listed in app/nyabula/tools/extra_glyphs.txt: the eye
 * fonts are subsets, built from the characters the generator is shown.
 */

#define NY_NET_EYE_STEP_JOIN  "1 扫码连接热点"
#define NY_NET_EYE_STEP_SETUP "2 扫码开始设置"
#define NY_NET_EYE_OPEN_PANEL "扫码打开控制面板"

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
  char name[NY_NET_SSID_MAX + 1]; /* what the owner calls it; "" = default */
  char ap_ssid[NY_NET_SSID_MAX + 1];
  char ap_psk[NY_NET_PSK_MAX + 1];
  int ap_channel;
  char sta_ssid[NY_NET_SSID_MAX + 1];
  char sta_psk[NY_NET_PSK_MAX + 1];
  char sta_ipv4[INET_ADDRSTRLEN];
  time_t sta_started;
  time_t sta_retry_at; /* no station attempt before this */
  time_t rejoin_at;    /* when to try the stored network again; 0 = never */
  time_t rssi_at;      /* when the signal level is read next */
  int sta_rssi;        /* dBm, 0 = not known */
  unsigned int rejoin_step;
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

/* Derive a per-device AP SSID from the MAC address. */

static void ny_net_default_ap_ssid(char *out, size_t size)
{
  uint8_t mac[6] = { 0 };
#ifdef CONFIG_NETUTILS_NETLIB
  netlib_getmacaddr(NY_NET_IFNAME, mac);
#endif
  snprintf(out, size, NY_NET_AP_SSID_FMT, mac[4], mac[5]);
}

/* The device has one name.  It is what the panel shows, what the access
 * point is called and what the router lists the device under.  Until the
 * owner chooses one it is the name derived from the MAC address, so the
 * thing on the desk, the network it offers and the entry in the router all
 * read the same (lock held).
 */

static void ny_net_name(char *out, size_t size)
{
  if (g_net.name[0] != 0)
    strlcpy(out, g_net.name, size);
  else
    ny_net_default_ap_ssid(out, size);
}

/* A name can be anything a person would type, within the 32 bytes an SSID
 * allows.  Control characters have no business in it.
 */

static bool ny_net_name_valid(const char *name)
{
  size_t length = strlen(name);
  if (length > NY_NET_SSID_MAX)
    return false;
  for (size_t i = 0; i < length; i++)
    if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f)
      return false;
  return true;
}

/* An SSID may hold any bytes, but this one also travels inside a WIFI: code
 * on the eyes, where ; , : " and \ are syntax, and phones disagree about
 * names that are not plain ASCII.  A name outside that set is still the
 * device's name; the access point then keeps the default one.
 */

static bool ny_net_name_is_ssid(const char *name)
{
  if (name[0] == 0 || name[0] == ' ')
    return false;
  for (const char *c = name; *c != 0; c++)
    if (*c < 0x20 || *c > 0x7e || strchr(";,:\"\\", *c) != NULL)
      return false;
  return true;
}

/* What DHCP tells the router: letters, digits and hyphens only, so
 * everything else becomes a hyphen and runs of them collapse.  A name with
 * nothing usable in it falls back to the default, which always qualifies
 * (lock held).
 */

static void ny_net_apply_hostname(void)
{
  char name[NY_NET_SSID_MAX + 1];
  char host[HOST_NAME_MAX + 1];
  size_t used = 0;
  ny_net_name(name, sizeof(name));
  for (int pass = 0; pass < 2 && used == 0; pass++)
    {
      for (const char *c = name; *c != 0 && used < HOST_NAME_MAX; c++)
        {
          bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                       (*c >= '0' && *c <= '9');
          if (plain)
            host[used++] = *c;
          else if (used > 0 && host[used - 1] != '-')
            host[used++] = '-';
        }

      while (used > 0 && host[used - 1] == '-')
        used--;
      if (used == 0)
        ny_net_default_ap_ssid(name, sizeof(name));
    }

  if (used > 0)
    sethostname(host, used);
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
    {
      ny_net_apply_hostname();
      return;
    }

  const char *name = ny_net_text(value, "name");
  if (name != NULL && ny_net_name_valid(name))
    strlcpy(g_net.name, name, sizeof(g_net.name));
  ny_net_apply_hostname();
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
  if (g_net.name[0] != 0)
    cJSON_AddStringToObject(value, "name", g_net.name);
  int ret = ny_product_store_write(NY_NET_STORE_DOMAIN, value, g_net.revision,
                                   &g_net.revision);
  cJSON_Delete(value);
  return ret;
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
    {
      if (ny_net_name_is_ssid(g_net.name))
        strlcpy(g_net.ap_ssid, g_net.name, sizeof(g_net.ap_ssid));
      else
        ny_net_default_ap_ssid(g_net.ap_ssid, sizeof(g_net.ap_ssid));
    }

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

  /* Say something.  The Wi-Fi firmware answers and filters ARP from the
   * addresses the driver last gave it, and the driver only passes them on
   * when it is about to transmit.  The last time it did was at link-up,
   * before this lease existed, so the firmware is still holding whatever
   * address the interface had then.  A device with a new lease and nothing
   * to send would stay that way: requests for its real address never reach
   * the host, and nobody can open a connection to it until it happens to
   * transmit on its own.  One datagram to the router's discard port is
   * enough -- resolving the router is itself a transmission.
   */

  if (ds.default_router.s_addr != 0)
    {
      int sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock >= 0)
        {
          struct sockaddr_in router = { 0 };
          router.sin_family = AF_INET;
          router.sin_port = htons(9);
          router.sin_addr = ds.default_router;
          sendto(sock, "", 1, MSG_DONTWAIT, (struct sockaddr *)&router,
                 sizeof(router));
          close(sock);
        }
    }

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

/* The signal level costs a firmware query, so it is read from the product
 * worker every few seconds and handed out from here.  Panels ask for it on
 * every page, from their own threads, and must not queue up behind the
 * radio.
 */

static void ny_net_rssi_poll(void)
{
  time_t now = time(NULL);
  int rssi = 0;
  bool online;

  if (nxmutex_lock(&g_net_lock) < 0)
    return;
  online = g_net.state == NY_NET_STA_ONLINE;
  if (!online)
    g_net.sta_rssi = 0;
  if (!online || now < g_net.rssi_at)
    {
      nxmutex_unlock(&g_net_lock);
      return;
    }

  g_net.rssi_at = now + NY_NET_RSSI_PERIOD_S;
  nxmutex_unlock(&g_net_lock);

#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  if (sock >= 0)
    {
      struct iwreq request;
      memset(&request, 0, sizeof(request));
      strlcpy(request.ifr_name, NY_NET_IFNAME, IFNAMSIZ);
      if (ioctl(sock, SIOCGIWSENS, (unsigned long)&request) >= 0 &&
          request.u.sens.value < 0)
        rssi = request.u.sens.value;
      close(sock);
    }
#endif

  if (nxmutex_lock(&g_net_lock) < 0)
    return;
  if (g_net.state == NY_NET_STA_ONLINE)
    g_net.sta_rssi = rssi;
  nxmutex_unlock(&g_net_lock);
}

/* --- Result builders (lock held) --- */

static cJSON *ny_net_status_json(bool owner)
{
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return NULL;
  char name[NY_NET_SSID_MAX + 1];
  ny_net_name(name, sizeof(name));
  cJSON_AddStringToObject(root, "name", name);
  cJSON_AddBoolToObject(root, "named", g_net.name[0] != 0);

  /* What the router is told, which can differ from the name: host names
   * are letters, digits and hyphens.
   */

  char host[HOST_NAME_MAX + 1];
  if (gethostname(host, sizeof(host)) == 0 && host[0] != 0)
    cJSON_AddStringToObject(root, "hostname", host);
  cJSON_AddStringToObject(root, "state", g_net_state_names[g_net.state]);
  cJSON_AddStringToObject(root, "ifname", NY_NET_IFNAME);
  cJSON_AddBoolToObject(root, "configured", g_net.sta_ssid[0] != '\0');
  cJSON_AddNumberToObject(root, "error", g_net.last_error);
  cJSON_AddNumberToObject(root, "attempt", g_net.sta_attempt);
  if (g_net.sta_ssid[0] != '\0')
    cJSON_AddStringToObject(root, "ssid", g_net.sta_ssid);
  if (g_net.sta_ipv4[0] != '\0')
    cJSON_AddStringToObject(root, "ipv4", g_net.sta_ipv4);
  if (g_net.state == NY_NET_STA_ONLINE && g_net.sta_rssi != 0)
    cJSON_AddNumberToObject(root, "rssi", g_net.sta_rssi);
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
        {
          if (ny_net_name_is_ssid(g_net.name))
            strlcpy(g_net.ap_ssid, g_net.name, sizeof(g_net.ap_ssid));
          else
            ny_net_default_ap_ssid(g_net.ap_ssid, sizeof(g_net.ap_ssid));
        }

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
      g_net.rejoin_at = 0;
      g_net.rejoin_step = 0;
      g_net.pending_sta = true;
      *result = ny_net_status_json(true);
      ret = *result ? 0 : -ENOMEM;
    }
  else if (strcmp(topic, "network.name.set") == 0)
    {
      const char *name = ny_net_text(data, "name");
      char previous[NY_NET_SSID_MAX + 1];
      if (name == NULL || !ny_net_name_valid(name))
        {
          ret = -EINVAL;
          goto out;
        }

      while (*name == ' ')
        name++;
      strlcpy(previous, g_net.name, sizeof(previous));
      strlcpy(g_net.name, name, sizeof(g_net.name));
      for (size_t end = strlen(g_net.name);
           end > 0 && g_net.name[end - 1] == ' '; end--)
        g_net.name[end - 1] = 0;
      ret = ny_net_save();
      if (ret < 0)
        {
          strlcpy(g_net.name, previous, sizeof(g_net.name));
          goto out;
        }

      /* The router learns the new name with the next lease; an access point
       * that is already up keeps the name people are connected to, and the
       * next one takes the new one.
       */

      ny_net_apply_hostname();
      if (g_net.state != NY_NET_AP_PROVISION)
        g_net.ap_ssid[0] = 0;
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

  /* Time to try the stored network again?  Not while somebody has the
   * panel open: taking the access point down would cut them off in the
   * middle of whatever they came to do, which is most likely to give the
   * device a different network.
   */

  if (g_net.state == NY_NET_AP_PROVISION && g_net.rejoin_at != 0 &&
      g_net.sta_ssid[0] != '\0' && !g_net.pending_ap && !g_net.pending_sta &&
      !g_net.pending_stop && time(NULL) >= g_net.rejoin_at)
    {
#ifdef CONFIG_NYABULA_CORE_WEB
      if (ny_web_panel_count() > 0)
        g_net.rejoin_at = time(NULL) + 60;
      else
#endif
        {
          g_net.rejoin_at = 0;
          g_net.pending_sta = true;
        }
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

      /* The driver joins a network from what its last scan saw, and says
       * -ENOENT for one it has not seen.  Right after boot it has seen
       * nothing, so the stored network failed every time and the device
       * came up as an access point although its network was there.  Look,
       * then ask again.
       */

      if (error == -ENOENT)
        {
          int scan_error = 0;
          cJSON_Delete(ny_net_hw_scan(&scan_error));
          error = ny_net_hw_sta_connect(ssid, psk);
        }

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
          g_net.rejoin_at = 0;
          g_net.rejoin_step = 0;
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
              /* Fall back, but not for good.  A network that did not answer
               * is more often late than gone -- after a power cut the
               * router takes longer to come up than this does -- so it is
               * tried again at growing intervals.
               */

              static const unsigned int delays[] = { 60, 120, 300, 600 };
              unsigned int last = sizeof(delays) / sizeof(delays[0]) - 1;
              unsigned int step =
                  g_net.rejoin_step < last ? g_net.rejoin_step : last;
              g_net.rejoin_at = time(NULL) + delays[step];
              g_net.rejoin_step++;
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
      /* The service refuses a command without a params object, also one
       * that has nothing to say, such as hiding a scene.
       */

      if (params == NULL)
        {
          params = cJSON_CreateObject();
        }

      if (params != NULL)
        {
          cJSON_AddItemToObject(command, "params", params);
          params = NULL;
          json = cJSON_PrintUnformatted(command);
        }
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
 * right eye, the access point's own join code on the left.  A phone scans
 * the left to get onto the device's network and the right to open the
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
    {
      cJSON_AddStringToObject(payload, "qr_left", text);
      cJSON_AddStringToObject(payload, "qr_left_label", NY_NET_EYE_STEP_JOIN);
    }
  memset(pass, 0, sizeof(pass));
  if (ny_web_product_token(token, sizeof(token)) == 0)
    {
      snprintf(text, sizeof(text),
               "http://" NY_NET_AP_ADDR "/#/provision?token=%s", token);
      cJSON_AddStringToObject(payload, "qr_right", text);
      cJSON_AddStringToObject(payload, "qr_right_label",
                              NY_NET_EYE_STEP_SETUP);
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
  static bool visible;
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

  /* The codes are an invitation, and once somebody has a panel open the
   * invitation has been taken up: the eyes go back to being eyes.  On the
   * access point they return if that panel goes away, since the device is
   * still waiting to be set up.  Once online the address has served its
   * purpose the first time anyone gets in, and is not shown again.
   */

  if (ny_web_panel_count() > 0 &&
      (state == NY_NET_AP_PROVISION || state == NY_NET_STA_ONLINE))
    {
      if (visible &&
          ny_net_eye_submit("eyes.scene.hide", NULL, NY_NET_EYE_LEASE_MS) == 0)
        {
          visible = false;
        }

      shown = state == NY_NET_STA_ONLINE ? NY_NET_STA_ONLINE : NY_NET_IDLE;
      return;
    }

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
        {
          shown = NY_NET_AP_PROVISION;
          visible = true;
        }
      else
        {
          shown = NY_NET_IDLE;
          visible = false;
        }
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
          cJSON_AddStringToObject(payload, "qr_right", text);
        else if (snprintf(text, sizeof(text), "http://%s/", ipv4) <
                 (int)sizeof(text))
          cJSON_AddStringToObject(payload, "qr_right", text);
        memset(token, 0, sizeof(token));
        memset(text, 0, sizeof(text));
      }

      cJSON_AddStringToObject(payload, "qr_right_label",
                              NY_NET_EYE_OPEN_PANEL);
      cJSON_AddStringToObject(payload, "title", "网络已连接");
      cJSON_AddStringToObject(payload, "detail", ipv4);
      cJSON_AddStringToObject(params, "scene", "qr");
      cJSON_AddStringToObject(params, "style", "full");
      cJSON_AddItemToObject(params, "payload", payload);
      if (ny_net_eye_submit("eyes.scene.show", params, NY_NET_EYE_ONLINE_MS) ==
          0)
        {
          shown = NY_NET_STA_ONLINE;
          visible = true;
        }
    }
  else if (shown != NY_NET_IDLE)
    {
      ny_net_eye_submit("eyes.scene.hide", NULL, NY_NET_EYE_LEASE_MS);
      shown = NY_NET_IDLE;
      visible = false;
    }
}
#endif

/****************************************************************************
 * Name: ny_product_network_tick
 ****************************************************************************/

int ny_product_network_tick(void)
{
  int ret = ny_net_step();
  ny_net_rssi_poll();
#ifdef CONFIG_NYABULA_CORE_TIMESYNC
  /* Online means the station holds a lease, which is the first moment a
   * time server can be reached.  On the access point there is no way out.
   */

  bool online = false;
  if (nxmutex_lock(&g_net_lock) == 0)
    {
      online = g_net.state == NY_NET_STA_ONLINE;
      nxmutex_unlock(&g_net_lock);
    }

  ny_product_timesync_tick(online);
#endif
#if defined(CONFIG_NYABULA_CORE_EYE) && defined(CONFIG_NYABULA_CORE_WEB)
  ny_net_eye_sync();
#endif
  return ret;
}

/****************************************************************************
 * Name: ny_product_network_name
 *
 * Description:
 *   The device's name, for whoever introduces the device to a client.
 *
 ****************************************************************************/

int ny_product_network_name(char *out, size_t size)
{
  int ret = nxmutex_lock(&g_net_lock);
  if (ret < 0)
    return ret;
  ny_net_load();
  ny_net_name(out, size);
  nxmutex_unlock(&g_net_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_network_link
 *
 * Description:
 *   The network the device is on, for the system summary: its name and the
 *   last signal level read.  Returns -ENOTCONN while not on a network;
 *   *rssi is 0 while the level is not known yet.
 *
 ****************************************************************************/

int ny_product_network_link(char *ssid, size_t size, int *rssi)
{
  int ret = nxmutex_lock(&g_net_lock);
  if (ret < 0)
    return ret;
  if (g_net.state != NY_NET_STA_ONLINE)
    ret = -ENOTCONN;
  else
    {
      strlcpy(ssid, g_net.sta_ssid, size);
      *rssi = g_net.sta_rssi;
    }

  nxmutex_unlock(&g_net_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_network_shutdown
 ****************************************************************************/

void ny_product_network_shutdown(void)
{
#ifdef CONFIG_NYABULA_CORE_TIMESYNC
  ny_product_timesync_shutdown();
#endif
  if (nxmutex_lock(&g_net_lock) < 0)
    return;
  bool ap = g_net.state == NY_NET_AP_PROVISION;
  g_net.state = NY_NET_IDLE;
  nxmutex_unlock(&g_net_lock);
  if (ap)
    ny_net_hw_ap_stop();
}
