/****************************************************************************
 * app/nyabula_core/ny_camera_gaze.h
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

#ifndef __APP_NYABULA_CORE_NY_CAMERA_GAZE_H
#define __APP_NYABULA_CORE_NY_CAMERA_GAZE_H

#define NY_CAMERA_GAZE_POINTS 5

/* Camera coordinates are normalized, unmirrored image coordinates [0,1].
 * Eye coordinates use the engine's [-1,1] range. Keep width/height/crop
 * identity with the calibration record; normalization cannot undo a crop.
 */

struct ny_camera_gaze_point_s
{
  double u;
  double v;
  double x;
  double y;
};

struct ny_camera_gaze_s
{
  double x[3];
  double y[3];
  double rms;
  double maximum;
};

int ny_camera_gaze_fit(const struct ny_camera_gaze_point_s points[5],
                       struct ny_camera_gaze_s *mapping);
int ny_camera_gaze_map(const struct ny_camera_gaze_s *mapping, double u,
                       double v, double *x, double *y);

#endif
