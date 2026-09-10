/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_signature.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <netutils/cJSON.h>
#include <sodium.h>

#include "ny_revocation.h"
#include "ny_signature.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_SIGNATURE_SIZE      64
#define NY_PUBLIC_KEY_SIZE     32
#define NY_HASH_SIZE           32
#define NY_HASH_HEX_SIZE       (NY_HASH_SIZE * 2)
#define NY_SIGNER_LIMIT        4096
#define NY_HASH_MANIFEST_LIMIT 65536
#define NY_TRUST_STORE_LIMIT   65536

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_verify_context_s
{
  const char *root;
  cJSON *hashes;
  size_t files;
  size_t bytes;
  size_t matched;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_signature_join(char *path, size_t size, const char *left,
                             const char *right);
static int ny_signature_read(const char *path, size_t limit,
                             unsigned char **content, size_t *length);
static cJSON *ny_signature_parse_json(const unsigned char *content,
                                      size_t length);
static int ny_signature_member_count(const cJSON *object, const char *name);
static int ny_signature_nibble(char value);
static int ny_signature_hex(const char *encoded, unsigned char *output,
                            size_t output_size);
static bool ny_signature_safe_relative(const char *path);
static bool ny_signature_mutable_directory(const char *name);
static int ny_signature_hash_file(const char *path, unsigned char digest[32],
                                  size_t *length);
static int ny_signature_verify_file(struct ny_verify_context_s *context,
                                    const char *absolute,
                                    const char *relative);
static int ny_signature_verify_directory(struct ny_verify_context_s *context,
                                         const char *absolute,
                                         const char *relative);
static int ny_signature_validate_hashes(cJSON *hashes);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_signature_join(char *path, size_t size, const char *left,
                             const char *right)
{
  int ret = snprintf(path, size, "%s/%s", left, right);

  return ret < 0 || ret >= (int)size ? -ENAMETOOLONG : 0;
}

static int ny_signature_read(const char *path, size_t limit,
                             unsigned char **content, size_t *length)
{
  FILE *stream;
  unsigned char *buffer;
  long size;
  size_t count;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fseek(stream, 0, SEEK_END) < 0 || (size = ftell(stream)) < 0 ||
      fseek(stream, 0, SEEK_SET) < 0)
    {
      int error = errno;
      fclose(stream);
      return -error;
    }

  if (size <= 0 || (size_t)size > limit)
    {
      fclose(stream);
      return -EFBIG;
    }

  buffer = malloc((size_t)size + 1);
  if (buffer == NULL)
    {
      fclose(stream);
      return -ENOMEM;
    }

  count = fread(buffer, 1, (size_t)size, stream);
  fclose(stream);
  if (count != (size_t)size)
    {
      free(buffer);
      return -EIO;
    }

  buffer[count] = '\0';
  *content = buffer;
  *length = count;
  return 0;
}

static cJSON *ny_signature_parse_json(const unsigned char *content,
                                      size_t length)
{
  const char *end;

  if (memchr(content, '\0', length) != NULL)
    {
      return NULL;
    }

  return cJSON_ParseWithOpts((const char *)content, &end, true);
}

static int ny_signature_member_count(const cJSON *object, const char *name)
{
  const cJSON *child;
  int count = 0;

  cJSON_ArrayForEach(child, object)
  {
    if (child->string != NULL && strcmp(child->string, name) == 0)
      {
        count++;
      }
  }

  return count;
}

static int ny_signature_hex(const char *encoded, unsigned char *output,
                            size_t output_size)
{
  size_t index;

  if (strlen(encoded) != output_size * 2)
    {
      return -EINVAL;
    }

  for (index = 0; index < output_size; index++)
    {
      int high = ny_signature_nibble(encoded[index * 2]);
      int low = ny_signature_nibble(encoded[index * 2 + 1]);

      if (high < 0 || low < 0)
        {
          return -EINVAL;
        }

      output[index] = (unsigned char)((high << 4) | low);
    }

  return 0;
}

static int ny_signature_nibble(char value)
{
  if (value >= '0' && value <= '9')
    {
      return value - '0';
    }

  if (value >= 'a' && value <= 'f')
    {
      return value - 'a' + 10;
    }

  if (value >= 'A' && value <= 'F')
    {
      return value - 'A' + 10;
    }

  return -1;
}

static bool ny_signature_safe_relative(const char *path)
{
  return path[0] != '\0' && path[0] != '/' && strchr(path, '\\') == NULL &&
         strstr(path, "//") == NULL && strcmp(path, ".") != 0 &&
         strcmp(path, "..") != 0 && strncmp(path, "../", 3) != 0 &&
         strstr(path, "/../") == NULL && strstr(path, "/./") == NULL;
}

static bool ny_signature_mutable_directory(const char *name)
{
  return strcmp(name, "data") == 0 || strcmp(name, "cache") == 0 ||
         strcmp(name, "tmp") == 0;
}

static int ny_signature_hash_file(const char *path, unsigned char digest[32],
                                  size_t *length)
{
  crypto_hash_sha256_state state;
  unsigned char buffer[1024];
  FILE *stream;
  size_t total = 0;
  size_t count;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      return -errno;
    }

  crypto_hash_sha256_init(&state);
  while ((count = fread(buffer, 1, sizeof(buffer), stream)) > 0)
    {
      total += count;
      if (total > CONFIG_NYABULA_CORE_PACKAGE_SIZE_LIMIT)
        {
          fclose(stream);
          return -EFBIG;
        }

      crypto_hash_sha256_update(&state, buffer, count);
    }

  if (ferror(stream))
    {
      fclose(stream);
      return -EIO;
    }

  fclose(stream);
  crypto_hash_sha256_final(&state, digest);
  *length = total;
  return 0;
}

static int ny_signature_verify_file(struct ny_verify_context_s *context,
                                    const char *absolute, const char *relative)
{
  unsigned char expected[NY_HASH_SIZE];
  unsigned char actual[NY_HASH_SIZE];
  const cJSON *item;
  size_t length = 0;
  int ret;

  item = cJSON_GetObjectItemCaseSensitive(context->hashes, relative);
  if (!cJSON_IsString(item) || item->valuestring == NULL ||
      ny_signature_member_count(context->hashes, relative) != 1)
    {
      return -EACCES;
    }

  ret = ny_signature_hex(item->valuestring, expected, sizeof(expected));
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_signature_hash_file(absolute, actual, &length);
  if (ret < 0 || sodium_memcmp(actual, expected, sizeof(actual)) != 0)
    {
      return ret < 0 ? ret : -EACCES;
    }

  context->files++;
  context->matched++;
  context->bytes += length;
  if (context->files > CONFIG_NYABULA_CORE_PACKAGE_FILE_LIMIT ||
      context->bytes > CONFIG_NYABULA_CORE_PACKAGE_SIZE_LIMIT)
    {
      return -EFBIG;
    }

  return 0;
}

static int ny_signature_verify_directory(struct ny_verify_context_s *context,
                                         const char *absolute,
                                         const char *relative)
{
  struct dirent *entry;
  DIR *directory;
  int ret = 0;

  directory = opendir(absolute);
  if (directory == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      struct stat status;
      char child_absolute[PATH_MAX];
      char child_relative[PATH_MAX];

      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      ret = ny_signature_join(child_absolute, sizeof(child_absolute), absolute,
                              entry->d_name);
      if (ret < 0)
        {
          break;
        }

      if (relative[0] == '\0')
        {
          strlcpy(child_relative, entry->d_name, sizeof(child_relative));
        }
      else
        {
          ret = ny_signature_join(child_relative, sizeof(child_relative),
                                  relative, entry->d_name);
          if (ret < 0)
            {
              break;
            }
        }

      if (!ny_signature_safe_relative(child_relative) ||
          lstat(child_absolute, &status) < 0 || S_ISLNK(status.st_mode))
        {
          ret = -EACCES;
          break;
        }

      if (relative[0] == '\0' && ny_signature_mutable_directory(entry->d_name))
        {
          if (!S_ISDIR(status.st_mode))
            {
              ret = -EACCES;
              break;
            }

          continue;
        }

      if (S_ISDIR(status.st_mode))
        {
          ret = ny_signature_verify_directory(context, child_absolute,
                                              child_relative);
        }
      else if (S_ISREG(status.st_mode))
        {
          if (strcmp(child_relative, "hashes.json") != 0 &&
              strcmp(child_relative, "signature.ed25519") != 0)
            {
              ret = ny_signature_verify_file(context, child_absolute,
                                             child_relative);
            }
        }
      else
        {
          ret = -EACCES;
        }

      if (ret < 0)
        {
          break;
        }
    }

  closedir(directory);
  return ret;
}

static int ny_signature_validate_hashes(cJSON *hashes)
{
  const cJSON *left;
  const cJSON *right;

  if (!cJSON_IsObject(hashes) ||
      cJSON_GetObjectItemCaseSensitive(hashes, "manifest.json") == NULL ||
      cJSON_GetObjectItemCaseSensitive(hashes, "signer.json") == NULL)
    {
      return -EINVAL;
    }

  cJSON_ArrayForEach(left, hashes)
  {
    if (left->string == NULL || !ny_signature_safe_relative(left->string) ||
        !cJSON_IsString(left) || left->valuestring == NULL ||
        strlen(left->valuestring) != NY_HASH_HEX_SIZE)
      {
        return -EINVAL;
      }

    for (right = left->next; right != NULL; right = right->next)
      {
        if (right->string != NULL && strcmp(left->string, right->string) == 0)
          {
            return -EINVAL;
          }
      }
  }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_signature_verify_package(const char *package_path)
{
  struct ny_verify_context_s context;
  unsigned char public_key[NY_PUBLIC_KEY_SIZE];
  unsigned char *hash_data = NULL;
  unsigned char *signature = NULL;
  unsigned char *signer_data = NULL;
  unsigned char *trust_data = NULL;
  const cJSON *item;
  cJSON *hashes = NULL;
  cJSON *signer = NULL;
  cJSON *trust = NULL;
  struct stat status;
  char path[PATH_MAX];
  size_t hash_length;
  size_t signature_length;
  size_t signer_length;
  size_t trust_length;
  int ret;

  if (package_path == NULL || package_path[0] == '\0' ||
      lstat(package_path, &status) < 0 || !S_ISDIR(status.st_mode) ||
      S_ISLNK(status.st_mode))
    {
      return -EINVAL;
    }

  ret = ny_signature_join(path, sizeof(path), package_path, "hashes.json");
  if (ret < 0 || (ret = ny_signature_read(path, NY_HASH_MANIFEST_LIMIT,
                                          &hash_data, &hash_length)) < 0)
    {
      goto out;
    }

  ret =
      ny_signature_join(path, sizeof(path), package_path, "signature.ed25519");
  if (ret < 0 ||
      (ret = ny_signature_read(path, NY_SIGNATURE_SIZE, &signature,
                               &signature_length)) < 0 ||
      signature_length != NY_SIGNATURE_SIZE)
    {
      ret = ret == -ENOENT ? -EACCES : ret < 0 ? ret : -EINVAL;
      goto out;
    }

  ret = ny_signature_join(path, sizeof(path), package_path, "signer.json");
  if (ret < 0 || (ret = ny_signature_read(path, NY_SIGNER_LIMIT, &signer_data,
                                          &signer_length)) < 0)
    {
      if (ret == -ENOENT)
        {
          ret = -EACCES;
        }

      goto out;
    }

  ret = ny_signature_read(CONFIG_NYABULA_CORE_TRUST_STORE,
                          NY_TRUST_STORE_LIMIT, &trust_data, &trust_length);
  if (ret < 0)
    {
      goto out;
    }

  hashes = ny_signature_parse_json(hash_data, hash_length);
  signer = ny_signature_parse_json(signer_data, signer_length);
  trust = ny_signature_parse_json(trust_data, trust_length);
  if (ny_signature_validate_hashes(hashes) < 0 || !cJSON_IsObject(signer) ||
      !cJSON_IsObject(trust) ||
      ny_signature_member_count(signer, "keyId") != 1)
    {
      ret = -EINVAL;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(signer, "keyId");
  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      ret = -EINVAL;
      goto out;
    }

  ret = ny_revocation_check_key(item->valuestring);
  if (ret < 0)
    {
      goto out;
    }

  if (ny_signature_member_count(trust, item->valuestring) != 1)
    {
      ret = -EACCES;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(trust, item->valuestring);
  if (!cJSON_IsString(item) || item->valuestring == NULL ||
      ny_signature_hex(item->valuestring, public_key, sizeof(public_key)) < 0)
    {
      ret = -EACCES;
      goto out;
    }

  if (sodium_init() < 0 ||
      crypto_sign_verify_detached(signature, hash_data, hash_length,
                                  public_key) != 0)
    {
      ret = -EACCES;
      goto out;
    }

  memset(&context, 0, sizeof(context));
  context.root = package_path;
  context.hashes = hashes;
  ret = ny_signature_verify_directory(&context, package_path, "");
  if (ret >= 0 && context.matched != (size_t)cJSON_GetArraySize(hashes))
    {
      ret = -EACCES;
    }

out:
  cJSON_Delete(trust);
  cJSON_Delete(signer);
  cJSON_Delete(hashes);
  free(trust_data);
  free(signer_data);
  free(signature);
  free(hash_data);
  return ret;
}
