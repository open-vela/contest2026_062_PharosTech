/****************************************************************************
 * tools/nyabula_core/tests/sntp_test.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
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

/* Host test of the SNTP arithmetic in app/nyabula_core/ny_sntp.c.  From the
 * repository root:
 *
 *   gcc -std=c99 -Wall -Wextra -Werror -Wshadow -Wundef -g \
 *       -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -Iapp/nyabula_core tools/nyabula_core/tests/sntp_test.c \
 *       app/nyabula_core/ny_sntp.c -o /tmp/sntp_test && /tmp/sntp_test
 */

#include "ny_sntp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TEST_TOKEN   0x5a17c3e9d2b4f681ULL
#define TEST_CEILING 4102444800000LL /* 2100-01-01 */

#define CHECK(condition)                                              \
  do                                                                  \
    {                                                                 \
      if (!(condition))                                               \
        {                                                             \
          printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
          g_failures++;                                               \
        }                                                             \
    }                                                                 \
  while (0)

static int g_failures;

/* A stratum 2 server's answer at 2026-09-19 08:00:00.250 UTC to a request
 * that carried TEST_TOKEN: LI 0, version 4, mode 4, poll 6, precision -25,
 * root delay and dispersion of a few tens of milliseconds, the upstream
 * server's address as the reference identifier, a reference timestamp 37 s
 * old, and the token echoed in the origin field.
 */

static const uint8_t g_valid_reply[NY_SNTP_PACKET_SIZE] = {
  0x24, 0x02, 0x06, 0xe7, 0x00, 0x00, 0x0a, 0x4c, 0x00, 0x00, 0x05, 0x2a,
  0x64, 0x64, 0x3d, 0x58, 0xee, 0x58, 0xbf, 0x5b, 0x1b, 0x2c, 0x3d, 0x4e,
  0x5a, 0x17, 0xc3, 0xe9, 0xd2, 0xb4, 0xf6, 0x81, 0xee, 0x58, 0xbf, 0x80,
  0x40, 0x00, 0x00, 0x00, 0xee, 0x58, 0xbf, 0x80, 0x40, 0x01, 0xa3, 0x6e
};

#define VALID_REPLY_MS 1789804800250LL

static void put_stamp(uint8_t *at, int64_t unix_seconds, uint32_t fraction);
static int64_t test_floor(void);
static void test_build(void);
static void test_valid(void);
static void test_rejected(void);
static void test_era(void);
static void test_now(void);
static void test_date(void);
static void test_servers(void);

/****************************************************************************
 * Name: put_stamp
 ****************************************************************************/

static void put_stamp(uint8_t *at, int64_t unix_seconds, uint32_t fraction)
{
  uint32_t seconds = (uint32_t)(unix_seconds + 2208988800LL);
  for (int i = 0; i < 4; i++)
    {
      at[i] = (uint8_t)(seconds >> (24 - 8 * i));
      at[4 + i] = (uint8_t)(fraction >> (24 - 8 * i));
    }
}

/****************************************************************************
 * Name: test_floor
 *
 * Description:
 *   The floor the firmware would use had it been built on 2026-09-19.
 *
 ****************************************************************************/

static int64_t test_floor(void)
{
  return ny_sntp_date_ms("Sep 19 2026") - 86400000LL;
}

/****************************************************************************
 * Name: test_build
 ****************************************************************************/

static void test_build(void)
{
  uint8_t packet[NY_SNTP_PACKET_SIZE];
  memset(packet, 0xff, sizeof(packet));
  ny_sntp_build(packet, TEST_TOKEN);
  CHECK(packet[0] == 0x23); /* LI 0, version 4, mode 3 */
  for (int i = 1; i < 40; i++)
    {
      CHECK(packet[i] == 0);
    }

  CHECK(memcmp(packet + 40, g_valid_reply + 24, 8) == 0);
}

/****************************************************************************
 * Name: test_valid
 ****************************************************************************/

static void test_valid(void)
{
  struct ny_sntp_reply_s reply;
  uint8_t longer[NY_SNTP_PACKET_SIZE + 20];
  uint8_t packet[NY_SNTP_PACKET_SIZE];

  CHECK(ny_sntp_parse(g_valid_reply, sizeof(g_valid_reply), TEST_TOKEN,
                      test_floor(), TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == VALID_REPLY_MS);
  CHECK(reply.receive_ms == VALID_REPLY_MS);
  CHECK(reply.stratum == 2 && reply.leap == 0 && reply.kiss[0] == '\0');

  /* Extension fields or a MAC after the header change nothing. */

  memset(longer, 0xa5, sizeof(longer));
  memcpy(longer, g_valid_reply, sizeof(g_valid_reply));
  CHECK(ny_sntp_parse(longer, sizeof(longer), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == VALID_REPLY_MS);

  /* Plenty of servers answer a version 4 request as version 3. */

  memcpy(packet, g_valid_reply, sizeof(packet));
  packet[0] = 0x1c;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);

  /* A leap second announcement is not an alarm. */

  packet[0] = 0x64;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.leap == 1);
}

/****************************************************************************
 * Name: test_rejected
 ****************************************************************************/

static void test_rejected(void)
{
  struct ny_sntp_reply_s reply;
  uint8_t packet[NY_SNTP_PACKET_SIZE];

  /* Kiss-o'-death: stratum 0, LI 3, "RATE", and no time worth the name. */

  memcpy(packet, g_valid_reply, sizeof(packet));
  packet[0] = 0xe4;
  packet[1] = 0;
  memcpy(packet + 12, "RATE", 4);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ECONNREFUSED);
  CHECK(strcmp(reply.kiss, "RATE") == 0);

  /* A code that is not text must not end up in a log as it is. */

  packet[12] = 0x07;
  packet[13] = 0xff;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ECONNREFUSED);
  CHECK(strcmp(reply.kiss, "??TE") == 0);

  /* Wrong origin: a reply to somebody else's request, or a forgery.  This
   * is judged before the kiss code, so a forged refusal is just ignored.
   */

  memcpy(packet, g_valid_reply, sizeof(packet));
  packet[31] ^= 1;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ESTALE);
  packet[1] = 0;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ESTALE);
  CHECK(reply.kiss[0] == '\0');

  /* A server that echoes zeros must not match a client without a token. */

  memset(packet + 24, 0, 8);
  CHECK(ny_sntp_parse(packet, sizeof(packet), 0, test_floor(), TEST_CEILING,
                      &reply) == -EINVAL);

  /* Short, empty, absent. */

  CHECK(ny_sntp_parse(g_valid_reply, NY_SNTP_PACKET_SIZE - 1, TEST_TOKEN,
                      test_floor(), TEST_CEILING, &reply) == -EMSGSIZE);
  CHECK(ny_sntp_parse(g_valid_reply, 0, TEST_TOKEN, test_floor(), TEST_CEILING,
                      &reply) == -EMSGSIZE);
  CHECK(ny_sntp_parse(NULL, NY_SNTP_PACKET_SIZE, TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EMSGSIZE);

  /* Not a server's reply: our own request reflected, a broadcast, NTPv2. */

  memcpy(packet, g_valid_reply, sizeof(packet));
  packet[0] = 0x23;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EPROTO);
  packet[0] = 0x25;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EPROTO);
  packet[0] = 0x14;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EPROTO);

  /* A server that says it does not know the time itself. */

  memcpy(packet, g_valid_reply, sizeof(packet));
  packet[0] = 0xe4;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EHOSTUNREACH);
  packet[0] = 0x24;
  packet[1] = 16;
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EHOSTUNREACH);

  /* No transmit timestamp at all. */

  memcpy(packet, g_valid_reply, sizeof(packet));
  memset(packet + 40, 0, 8);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -EINVAL);

  /* A time from before the firmware was built: 2021-01-01, which is what
   * the board's RTC was found counting from.  Then the last millisecond
   * before the floor, and the floor itself.
   */

  memcpy(packet, g_valid_reply, sizeof(packet));
  put_stamp(packet + 32, 1609459200, 0);
  put_stamp(packet + 40, 1609459200, 0);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ERANGE);
  put_stamp(packet + 40, test_floor() / 1000 - 1, 0xffffffffu);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == -ERANGE);
  put_stamp(packet + 40, test_floor() / 1000, 0);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == test_floor());

  /* And one from an impossible future. */

  CHECK(ny_sntp_parse(g_valid_reply, sizeof(g_valid_reply), TEST_TOKEN,
                      test_floor(), VALID_REPLY_MS - 1, &reply) == -ERANGE);
}

/****************************************************************************
 * Name: test_era
 *
 * Description:
 *   The seconds field wraps on 2036-02-07.  2040 must come out as 2040 and
 *   not as 1903.
 *
 ****************************************************************************/

static void test_era(void)
{
  struct ny_sntp_reply_s reply;
  uint8_t packet[NY_SNTP_PACKET_SIZE];
  memcpy(packet, g_valid_reply, sizeof(packet));
  put_stamp(packet + 32, 2208988800LL, 0);
  put_stamp(packet + 40, 2208988800LL, 0x80000000u);
  CHECK(packet[40] == 0x07); /* The top bit is clear: the second era */
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == 2208988800500LL);

  /* The last second of the first era and the first of the second. */

  put_stamp(packet + 40, 2085978495LL, 0);
  CHECK(packet[40] == 0xff);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == 2085978495000LL);
  put_stamp(packet + 40, 2085978496LL, 1);
  CHECK(packet[40] == 0x00);
  CHECK(ny_sntp_parse(packet, sizeof(packet), TEST_TOKEN, test_floor(),
                      TEST_CEILING, &reply) == 0);
  CHECK(reply.transmit_ms == 2085978496000LL);
}

/****************************************************************************
 * Name: test_now
 ****************************************************************************/

static void test_now(void)
{
  struct ny_sntp_reply_s reply;
  memset(&reply, 0, sizeof(reply));
  reply.receive_ms = 1000000;
  reply.transmit_ms = 1000010;

  /* 50 ms there and back, 10 of them spent in the server: 20 each way. */

  CHECK(ny_sntp_now_ms(&reply, 50) == 1000030);
  CHECK(ny_sntp_now_ms(&reply, 0) == 1000010);

  /* A server that claims to have held the request for longer than the
   * whole exchange took, or to have answered before it was asked, gets no
   * credit for it.
   */

  CHECK(ny_sntp_now_ms(&reply, 8) == 1000014);
  reply.receive_ms = 2000000;
  CHECK(ny_sntp_now_ms(&reply, 50) == 1000035);
}

/****************************************************************************
 * Name: test_date
 ****************************************************************************/

static void test_date(void)
{
  CHECK(ny_sntp_date_ms("Jan  1 1970") == 0);
  CHECK(ny_sntp_date_ms("Sep 19 2026") == 1789776000000LL);
  CHECK(ny_sntp_date_ms("Sep  9 2026") == 1788912000000LL);
  CHECK(ny_sntp_date_ms("Feb 29 2024") == 1709164800000LL);
  CHECK(ny_sntp_date_ms("Mar  1 2024") == 1709251200000LL);
  CHECK(ny_sntp_date_ms("Dec 31 2099") == 4102358400000LL);
  CHECK(ny_sntp_date_ms("Jan  1 2021") == 1609459200000LL);

  CHECK(ny_sntp_date_ms(NULL) == -1);
  CHECK(ny_sntp_date_ms("") == -1);
  CHECK(ny_sntp_date_ms("Sep 1 2026") == -1);
  CHECK(ny_sntp_date_ms("Foo 19 2026") == -1);
  CHECK(ny_sntp_date_ms("Sep 1x 2026") == -1);
  CHECK(ny_sntp_date_ms("Sep 00 2026") == -1);
  CHECK(ny_sntp_date_ms("Sep 32 2026") == -1);
  CHECK(ny_sntp_date_ms("Sep 19 20x6") == -1);
  CHECK(ny_sntp_date_ms("Sep 19 1969") == -1);
  CHECK(ny_sntp_date_ms("??? ?? ????") == -1);

  /* Whatever this compiler calls today has to be a date. */

  CHECK(ny_sntp_date_ms(__DATE__) > 1577836800000LL);
}

/****************************************************************************
 * Name: test_servers
 ****************************************************************************/

static void test_servers(void)
{
  static const char defaults[] =
      "ntp.aliyun.com;ntp.tencent.com;cn.pool.ntp.org;pool.ntp.org";
  char host[15];
  char wide[64];

  CHECK(ny_sntp_server(defaults, 0, wide, sizeof(wide)) == 0 &&
        strcmp(wide, "ntp.aliyun.com") == 0);
  CHECK(ny_sntp_server(defaults, 3, wide, sizeof(wide)) == 0 &&
        strcmp(wide, "pool.ntp.org") == 0);
  CHECK(ny_sntp_server(defaults, 4, wide, sizeof(wide)) == -ENOENT);

  CHECK(ny_sntp_server(" a;b, c\t10.0.0.1;;", 0, host, sizeof(host)) == 0 &&
        strcmp(host, "a") == 0);
  CHECK(ny_sntp_server(" a;b, c\t10.0.0.1;;", 2, host, sizeof(host)) == 0 &&
        strcmp(host, "c") == 0);
  CHECK(ny_sntp_server(" a;b, c\t10.0.0.1;;", 3, host, sizeof(host)) == 0 &&
        strcmp(host, "10.0.0.1") == 0);
  CHECK(ny_sntp_server(" a;b, c\t10.0.0.1;;", 4, host, sizeof(host)) ==
        -ENOENT);

  /* An entry that does not fit is skipped by the caller, not cut short. */

  CHECK(ny_sntp_server(defaults, 1, host, sizeof(host)) == -ENAMETOOLONG);
  CHECK(ny_sntp_server(defaults, 0, host, sizeof(host)) == 0);
  CHECK(ny_sntp_server("12345678901234", 0, host, sizeof(host)) == 0);
  CHECK(ny_sntp_server("123456789012345", 0, host, sizeof(host)) ==
        -ENAMETOOLONG);

  CHECK(ny_sntp_server("", 0, host, sizeof(host)) == -ENOENT);
  CHECK(ny_sntp_server(";; ;", 0, host, sizeof(host)) == -ENOENT);
  CHECK(ny_sntp_server(NULL, 0, host, sizeof(host)) == -EINVAL);
  CHECK(ny_sntp_server(defaults, 0, host, 0) == -EINVAL);
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(void)
{
  test_build();
  test_valid();
  test_rejected();
  test_era();
  test_now();
  test_date();
  test_servers();
  if (g_failures != 0)
    {
      printf("SNTP_TEST_FAIL %d\n", g_failures);
      return 1;
    }

  printf("SNTP_TEST_PASS build valid kiss origin short range era now date "
         "servers\n");
  return 0;
}
