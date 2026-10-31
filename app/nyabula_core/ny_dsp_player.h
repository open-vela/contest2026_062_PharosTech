/****************************************************************************
 * app/nyabula_core/ny_dsp_player.h
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

#ifndef __NYABULA_CORE_NY_DSP_PLAYER_H
#define __NYABULA_CORE_NY_DSP_PLAYER_H
#include <stdint.h>
struct ny_dsp_player_s;
struct ny_dsp_player_s *ny_dsp_player_create(void);
void ny_dsp_player_release(struct ny_dsp_player_s *player);
int ny_dsp_player_setdevice(struct ny_dsp_player_s *player, const char *path);
int ny_dsp_player_setvolume(struct ny_dsp_player_s *player,
                            unsigned int volume);
int ny_dsp_player_playpcmfd(struct ny_dsp_player_s *player, int fd,
                            uint8_t channels, uint8_t bits, uint32_t rate);
int ny_dsp_player_getstate(struct ny_dsp_player_s *player);
int ny_dsp_player_error(struct ny_dsp_player_s *player);
int ny_dsp_player_pause(struct ny_dsp_player_s *player);
int ny_dsp_player_resume(struct ny_dsp_player_s *player);
int ny_dsp_player_stop(struct ny_dsp_player_s *player);
#endif
