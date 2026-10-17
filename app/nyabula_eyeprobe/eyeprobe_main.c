/****************************************************************************
 * app/nyabula_eyeprobe/eyeprobe_main.c
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

#include <nuttx/config.h>
#include <nuttx/fs/fs.h>
#include <nuttx/lcd/lcd_dev.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NY_EYEPROBE_EYES   2
#define NY_EYEPROBE_WIDTH  360
#define NY_EYEPROBE_HEIGHT 360
#define NY_EYEPROBE_STRIDE (NY_EYEPROBE_WIDTH * 2)

static int ny_eyeprobe_ioctl(struct file *filep, int cmd, unsigned long arg);
static int ny_eyeprobe_save(const char *directory, unsigned int eye);

static pthread_mutex_t g_ny_eyeprobe_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_ny_eyeprobe_pixels[NY_EYEPROBE_EYES]
                                   [NY_EYEPROBE_HEIGHT * NY_EYEPROBE_STRIDE];
static unsigned int g_ny_eyeprobe_frames[NY_EYEPROBE_EYES];
static const struct file_operations g_ny_eyeprobe_ops = {
  .ioctl = ny_eyeprobe_ioctl
};

/* Required board hooks. The sim has no hardware LCD; the test explicitly
 * registers the two capture sinks with `eyeprobe init` instead.
 */

int board_lcd_initialize(void) { return -ENODEV; }

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  (void)devno;
  return NULL;
}

/****************************************************************************
 * Name: ny_eyeprobe_ioctl
 ****************************************************************************/

static int ny_eyeprobe_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  unsigned int eye = (uintptr_t)filep->f_inode->i_private;

  if (cmd == LCDDEVIO_SETPOWER)
    {
      return OK;
    }

  if (cmd == LCDDEVIO_GETAREAALIGN)
    {
      struct lcddev_area_align_s *align = (void *)arg;
      align->col_start_align = 1;
      align->row_start_align = 1;
      align->width_align = 1;
      align->height_align = 1;
      return OK;
    }

  if (cmd == LCDDEVIO_PUTAREA)
    {
      const struct lcddev_area_s *area = (void *)arg;
      if (area->row_start != 0 || area->col_start != 0 ||
          area->row_end != NY_EYEPROBE_HEIGHT - 1 ||
          area->col_end != NY_EYEPROBE_WIDTH - 1 ||
          area->stride != NY_EYEPROBE_STRIDE || area->data == NULL)
        {
          return -EINVAL;
        }

      pthread_mutex_lock(&g_ny_eyeprobe_lock);
      memcpy(g_ny_eyeprobe_pixels[eye], area->data,
             sizeof(g_ny_eyeprobe_pixels[eye]));
      g_ny_eyeprobe_frames[eye]++;
      pthread_mutex_unlock(&g_ny_eyeprobe_lock);
      return OK;
    }

  return -ENOTTY;
}

/****************************************************************************
 * Name: ny_eyeprobe_save
 ****************************************************************************/

static int ny_eyeprobe_save(const char *directory, unsigned int eye)
{
  char path[256];
  FILE *file;
  size_t written;
  int length;
  int ret;

  length = snprintf(path, sizeof(path), "%s/%s.rgb", directory,
                    eye == 0 ? "left" : "right");
  if (length < 0 || (size_t)length >= sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  file = fopen(path, "wb");
  if (file == NULL)
    {
      return -errno;
    }

  written = fwrite(g_ny_eyeprobe_pixels[eye], 1,
                   sizeof(g_ny_eyeprobe_pixels[eye]), file);
  ret = fclose(file);
  return written == sizeof(g_ny_eyeprobe_pixels[eye]) && ret == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, char **argv)
{
  unsigned int eye;
  int ret = OK;

  if (argc == 2 && strcmp(argv[1], "init") == 0)
    {
      ret = register_driver("/dev/lcd0", &g_ny_eyeprobe_ops, 0666, (void *)0);
      if (ret == OK)
        {
          ret = register_driver("/dev/lcd1", &g_ny_eyeprobe_ops, 0666,
                                (void *)1);
          if (ret < 0)
            {
              unregister_driver("/dev/lcd0");
            }
        }

      printf("PROBE_INIT result=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  if (argc != 1 && (argc != 3 || strcmp(argv[1], "save") != 0))
    {
      fprintf(stderr, "usage: eyeprobe [init|save DIRECTORY]\n");
      return 1;
    }

  pthread_mutex_lock(&g_ny_eyeprobe_lock);
  for (eye = 0; eye < NY_EYEPROBE_EYES; eye++)
    {
      unsigned int nonzero = 0;
      size_t i;

      for (i = 0; i < sizeof(g_ny_eyeprobe_pixels[eye]); i++)
        {
          nonzero += g_ny_eyeprobe_pixels[eye][i] != 0;
        }

      printf("EYE_FRAME sid=%u count=%u nonzero=%u\n", eye,
             g_ny_eyeprobe_frames[eye], nonzero);
      if (argc == 3 && ny_eyeprobe_save(argv[2], eye) < 0)
        {
          ret = -EIO;
        }
    }

  pthread_mutex_unlock(&g_ny_eyeprobe_lock);
  return ret < 0 ? 1 : 0;
}
