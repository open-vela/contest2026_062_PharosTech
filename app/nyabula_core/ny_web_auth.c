/****************************************************************************
 * app/nyabula_core/ny_web_auth.c
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

#include "ny_web_auth.h"

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_CRYPTO_MBEDTLS

/* Without a key-derivation function there is nothing safe to store, so a
 * build that leaves mbedTLS out has no password: the panel is opened with
 * the pair token only, as it was before passwords existed.
 */

int ny_web_auth_load(const char *path)
{
  (void)path;
  return 0;
}

bool ny_web_auth_has_password(void)
{
  return false;
}

bool ny_web_auth_locked(uint32_t *retry_after_ms)
{
  if (retry_after_ms != NULL)
    {
      *retry_after_ms = 0;
    }

  return false;
}

int ny_web_auth_check(const char *password, uint32_t *retry_after_ms)
{
  (void)password;
  if (retry_after_ms != NULL)
    {
      *retry_after_ms = 0;
    }

  return -ENOENT;
}

int ny_web_auth_set(const char *password)
{
  (void)password;
  return -ENOSYS;
}

int ny_web_auth_session_token(const char *pair_token, char *out)
{
  (void)pair_token;
  (void)out;
  return -ENOENT;
}

#else /* CONFIG_CRYPTO_MBEDTLS */

#include <mbedtls/constant_time.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_AUTH_SALT_SIZE   16
#define NY_AUTH_HASH_SIZE   32
#define NY_AUTH_ITERATIONS  20000
#define NY_AUTH_PATH_MAX    96
#define NY_AUTH_LABEL       "nyabula-session-v1"

/* Five wrong guesses are free: people mistype.  After that each further
 * one doubles the wait, from half a minute up to ten, which leaves an
 * owner who forgot their password mildly inconvenienced and someone
 * guessing over the network with a few hundred tries a day.
 */

#define NY_AUTH_FREE_TRIES  5
#define NY_AUTH_LOCK_MIN_MS 30000
#define NY_AUTH_LOCK_MAX_MS 600000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_auth_s
{
  char path[NY_AUTH_PATH_MAX];
  bool present;
  uint32_t iterations;
  uint8_t salt[NY_AUTH_SALT_SIZE];
  uint8_t hash[NY_AUTH_HASH_SIZE];
  unsigned int failures;
  uint64_t locked_until_ms;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_auth_now_ms(void);
static void ny_auth_hex(const uint8_t *in, size_t size, char *out);
static int ny_auth_unhex(const char *in, uint8_t *out, size_t size);
static int ny_auth_derive(const char *password, const uint8_t *salt,
                          uint32_t iterations, uint8_t *hash);
static int ny_auth_store(void);
static uint32_t ny_auth_remaining_ms(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_auth_lock = NXMUTEX_INITIALIZER;
static struct ny_auth_s g_auth;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t ny_auth_now_ms(void)
{
  struct timespec now;

  /* Monotonic, so that setting the clock neither lifts a lock nor extends
   * one.
   */

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void ny_auth_hex(const uint8_t *in, size_t size, char *out)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; i++)
    {
      out[i * 2] = digits[in[i] >> 4];
      out[i * 2 + 1] = digits[in[i] & 0x0f];
    }

  out[size * 2] = '\0';
}

static int ny_auth_unhex(const char *in, uint8_t *out, size_t size)
{
  for (size_t i = 0; i < size * 2; i++)
    {
      char c = in[i];
      uint8_t nibble;
      if (c >= '0' && c <= '9')
        {
          nibble = c - '0';
        }
      else if (c >= 'a' && c <= 'f')
        {
          nibble = c - 'a' + 10;
        }
      else
        {
          return -EINVAL;
        }

      out[i / 2] = (i & 1) ? (out[i / 2] | nibble) : (nibble << 4);
    }

  return 0;
}

static int ny_auth_derive(const char *password, const uint8_t *salt,
                          uint32_t iterations, uint8_t *hash)
{
  int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password),
      salt, NY_AUTH_SALT_SIZE, iterations, NY_AUTH_HASH_SIZE, hash);
  return ret == 0 ? 0 : -EIO;
}

/****************************************************************************
 * Name: ny_auth_store
 *
 * Description:
 *   Write the record (lock held).  It is written complete under a second
 *   name before the first is touched, so a power cut never leaves half a
 *   record as the only copy.  FAT cannot rename onto an existing name, so
 *   there is a moment with only the new file present; ny_web_auth_load()
 *   looks for it when the usual name is missing.
 *
 ****************************************************************************/

static int ny_auth_store(void)
{
  char temporary[NY_AUTH_PATH_MAX + 8];
  char salt[NY_AUTH_SALT_SIZE * 2 + 1];
  char hash[NY_AUTH_HASH_SIZE * 2 + 1];
  FILE *file;
  bool written;

  snprintf(temporary, sizeof(temporary), "%s.new", g_auth.path);
  file = fopen(temporary, "w");
  if (file == NULL)
    {
      return -errno;
    }

  ny_auth_hex(g_auth.salt, sizeof(g_auth.salt), salt);
  ny_auth_hex(g_auth.hash, sizeof(g_auth.hash), hash);
  written = fprintf(file, "v1 %lu %s %s\n", (unsigned long)g_auth.iterations,
                    salt, hash) > 0;
  written = fclose(file) == 0 && written;
  memset(hash, 0, sizeof(hash));
  if (!written)
    {
      unlink(temporary);
      return -EIO;
    }

  /* FAT rename does not replace an existing name. */

  unlink(g_auth.path);
  if (rename(temporary, g_auth.path) < 0)
    {
      return -errno;
    }

  return 0;
}

static uint32_t ny_auth_remaining_ms(void)
{
  uint64_t now = ny_auth_now_ms();
  return g_auth.locked_until_ms > now
             ? (uint32_t)(g_auth.locked_until_ms - now)
             : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_web_auth_load(const char *path)
{
  char salt[NY_AUTH_SALT_SIZE * 2 + 1];
  char hash[NY_AUTH_HASH_SIZE * 2 + 1];
  unsigned long iterations = 0;
  FILE *file;
  int ret;

  if (path == NULL || strlen(path) >= NY_AUTH_PATH_MAX)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_auth_lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(&g_auth, 0, sizeof(g_auth));
  strlcpy(g_auth.path, path, sizeof(g_auth.path));
  file = fopen(path, "r");
  if (file == NULL)
    {
      /* Interrupted between removing the old record and renaming the new
       * one into place: the new one is complete, so finish the job.
       */

      char temporary[NY_AUTH_PATH_MAX + 8];
      snprintf(temporary, sizeof(temporary), "%s.new", path);
      if (rename(temporary, path) == 0)
        {
          file = fopen(path, "r");
        }
    }

  if (file != NULL)
    {
      /* A record that does not parse is treated as absent rather than as a
       * reason to refuse everyone: the way back in is the code on the
       * eyes, which needs no password.
       */

      if (fscanf(file, "v1 %lu %32s %64s", &iterations, salt, hash) == 3 &&
          iterations >= 1000 && iterations <= 10000000 &&
          strlen(salt) == NY_AUTH_SALT_SIZE * 2 &&
          strlen(hash) == NY_AUTH_HASH_SIZE * 2 &&
          ny_auth_unhex(salt, g_auth.salt, NY_AUTH_SALT_SIZE) == 0 &&
          ny_auth_unhex(hash, g_auth.hash, NY_AUTH_HASH_SIZE) == 0)
        {
          g_auth.iterations = iterations;
          g_auth.present = true;
        }

      fclose(file);
      memset(hash, 0, sizeof(hash));
    }

  nxmutex_unlock(&g_auth_lock);
  return 0;
}

bool ny_web_auth_has_password(void)
{
  bool present;
  if (nxmutex_lock(&g_auth_lock) < 0)
    {
      return false;
    }

  present = g_auth.present;
  nxmutex_unlock(&g_auth_lock);
  return present;
}

bool ny_web_auth_locked(uint32_t *retry_after_ms)
{
  uint32_t remaining = 0;
  if (nxmutex_lock(&g_auth_lock) == 0)
    {
      remaining = ny_auth_remaining_ms();
      nxmutex_unlock(&g_auth_lock);
    }

  if (retry_after_ms != NULL)
    {
      *retry_after_ms = remaining;
    }

  return remaining > 0;
}

int ny_web_auth_check(const char *password, uint32_t *retry_after_ms)
{
  uint8_t hash[NY_AUTH_HASH_SIZE];
  uint32_t remaining;
  int ret;

  if (retry_after_ms != NULL)
    {
      *retry_after_ms = 0;
    }

  if (password == NULL)
    {
      return -EACCES;
    }

  ret = nxmutex_lock(&g_auth_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_auth.present)
    {
      nxmutex_unlock(&g_auth_lock);
      return -ENOENT;
    }

  /* A locked device does not even look at the guess, so that the wait
   * cannot be used to keep testing passwords and reading the timing.
   */

  remaining = ny_auth_remaining_ms();
  if (remaining > 0)
    {
      if (retry_after_ms != NULL)
        {
          *retry_after_ms = remaining;
        }

      nxmutex_unlock(&g_auth_lock);
      return -EAGAIN;
    }

  ret = strlen(password) <= NY_WEB_AUTH_PASSWORD_MAX
            ? ny_auth_derive(password, g_auth.salt, g_auth.iterations, hash)
            : -EACCES;
  if (ret == 0 &&
      mbedtls_ct_memcmp(hash, g_auth.hash, NY_AUTH_HASH_SIZE) == 0)
    {
      g_auth.failures = 0;
    }
  else
    {
      ret = ret == -EIO ? -EIO : -EACCES;
      if (ret == -EACCES && ++g_auth.failures > NY_AUTH_FREE_TRIES)
        {
          unsigned int step = g_auth.failures - NY_AUTH_FREE_TRIES - 1;
          uint64_t wait = NY_AUTH_LOCK_MIN_MS;
          while (step-- > 0 && wait < NY_AUTH_LOCK_MAX_MS)
            {
              wait *= 2;
            }

          if (wait > NY_AUTH_LOCK_MAX_MS)
            {
              wait = NY_AUTH_LOCK_MAX_MS;
            }

          g_auth.locked_until_ms = ny_auth_now_ms() + wait;
          if (retry_after_ms != NULL)
            {
              *retry_after_ms = (uint32_t)wait;
            }
        }
    }

  memset(hash, 0, sizeof(hash));
  nxmutex_unlock(&g_auth_lock);
  return ret;
}

int ny_web_auth_set(const char *password)
{
  struct ny_auth_s previous;
  size_t length = password != NULL ? strlen(password) : 0;
  int fd;
  int ret;

  if (length < NY_WEB_AUTH_PASSWORD_MIN || length > NY_WEB_AUTH_PASSWORD_MAX)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_auth_lock);
  if (ret < 0)
    {
      return ret;
    }

  previous = g_auth;
  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0 ||
      read(fd, g_auth.salt, sizeof(g_auth.salt)) != sizeof(g_auth.salt))
    {
      ret = -EIO;
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (ret == 0)
    {
      g_auth.iterations = NY_AUTH_ITERATIONS;
      ret = ny_auth_derive(password, g_auth.salt, g_auth.iterations,
                           g_auth.hash);
    }

  if (ret == 0)
    {
      g_auth.present = true;
      g_auth.failures = 0;
      g_auth.locked_until_ms = 0;
      ret = ny_auth_store();
    }

  /* A password that could not be stored is not adopted either.  Accepting
   * it until the next restart and then reverting would lock the owner out
   * with a password they were told had been changed.
   */

  if (ret < 0)
    {
      g_auth = previous;
    }

  memset(&previous, 0, sizeof(previous));
  nxmutex_unlock(&g_auth_lock);
  return ret;
}

int ny_web_auth_session_token(const char *pair_token, char *out)
{
  uint8_t message[sizeof(NY_AUTH_LABEL) - 1 + NY_AUTH_HASH_SIZE];
  uint8_t digest[32];
  int ret;

  if (pair_token == NULL || strlen(pair_token) != NY_WEB_AUTH_TOKEN_SIZE)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_auth_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_auth.present)
    {
      nxmutex_unlock(&g_auth_lock);
      return -ENOENT;
    }

  memcpy(message, NY_AUTH_LABEL, sizeof(NY_AUTH_LABEL) - 1);
  memcpy(message + sizeof(NY_AUTH_LABEL) - 1, g_auth.hash,
         NY_AUTH_HASH_SIZE);
  nxmutex_unlock(&g_auth_lock);

  ret = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        (const unsigned char *)pair_token,
                        NY_WEB_AUTH_TOKEN_SIZE, message, sizeof(message),
                        digest);
  memset(message, 0, sizeof(message));
  if (ret != 0)
    {
      return -EIO;
    }

  ny_auth_hex(digest, sizeof(digest), out);
  memset(digest, 0, sizeof(digest));
  return 0;
}

#endif /* CONFIG_CRYPTO_MBEDTLS */
