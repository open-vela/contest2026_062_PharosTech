/****************************************************************************
 * app/nyabula_core/ny_sntp.h
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

#ifndef __NYABULA_CORE_NY_SNTP_H
#define __NYABULA_CORE_NY_SNTP_H

/* The part of SNTP (RFC 4330) that is arithmetic: building the request,
 * judging the reply and turning it into a time.  Nothing in here touches a
 * socket, a clock or the operating system, so the same file is compiled
 * into the firmware and into a test program on the build machine.
 */

#include <stddef.h>
#include <stdint.h>

#define NY_SNTP_PACKET_SIZE 48
#define NY_SNTP_PORT        123

struct ny_sntp_reply_s
{
  int64_t receive_ms;  /* When the server got the request, Unix ms */
  int64_t transmit_ms; /* When the server sent this reply, Unix ms */
  uint8_t stratum;
  uint8_t leap;
  char kiss[5]; /* Kiss-o'-death code of a stratum 0 reply, or "" */
};

/* Fill in a client request.  The token travels in the transmit timestamp,
 * which the server copies into the origin field of its reply; it is what
 * ties a reply to this request, so it must be non-zero and hard to guess.
 */

void ny_sntp_build(uint8_t packet[NY_SNTP_PACKET_SIZE], uint64_t token);

/* Judge a datagram as the reply to the request that carried the token.
 * Returns 0 and fills in the reply, or:
 *   -EMSGSIZE      shorter than an NTP header
 *   -EPROTO        not a version 3/4 server reply
 *   -ESTALE        the origin field does not echo the token
 *   -ECONNREFUSED  stratum 0, a kiss-o'-death; reply->kiss holds the code
 *   -EHOSTUNREACH  the server says its own clock is not synchronised
 *   -EINVAL        no transmit timestamp, or a zero token
 *   -ERANGE        a time outside [floor_ms, ceiling_ms]
 */

int ny_sntp_parse(const uint8_t *packet, size_t length, uint64_t token,
                  int64_t floor_ms, int64_t ceiling_ms,
                  struct ny_sntp_reply_s *reply);

/* The time at the moment the reply arrived, given how long the exchange
 * took on the local monotonic clock.
 */

int64_t ny_sntp_now_ms(const struct ny_sntp_reply_s *reply,
                       uint64_t roundtrip_ms);

/* Midnight UTC of a compiler __DATE__ string ("Sep  9 2026") as Unix
 * milliseconds, or -1 when the text is not such a date.
 */

int64_t ny_sntp_date_ms(const char *date);

/* Entry number index of a server list separated by semicolons, commas or
 * blanks.  Returns 0, -ENOENT past the end, -ENAMETOOLONG when the entry
 * does not fit (the caller moves on to the next index).
 */

int ny_sntp_server(const char *list, unsigned int index, char *out,
                   size_t size);

#endif /* __NYABULA_CORE_NY_SNTP_H */
