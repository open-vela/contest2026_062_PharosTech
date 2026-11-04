/****************************************************************************
 * app/nyabula_core/ny_camera_gaze.c
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


#include "ny_camera_gaze.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>

#define NY_CAMERA_GAZE_PIVOT 0.000001
#define NY_CAMERA_GAZE_SPAN  0.2

static double ny_camera_gaze_clamp(double value);

/****************************************************************************
 * Name: ny_camera_gaze_clamp
 ****************************************************************************/

static double ny_camera_gaze_clamp(double value)
{
  return fmax(-1.0, fmin(1.0, value));
}

/****************************************************************************
 * Name: ny_camera_gaze_fit
 ****************************************************************************/

int ny_camera_gaze_fit(const struct ny_camera_gaze_point_s points[5],
                       struct ny_camera_gaze_s *mapping)
{
  struct ny_camera_gaze_s candidate = {0};
  double a[3][5] = {{0}};
  double min_u = 1;
  double min_v = 1;
  double max_u = 0;
  double max_v = 0;
  double min_x = 1, min_y = 1, max_x = -1, max_y = -1;
  if (points == NULL || mapping == NULL)
    return -EINVAL;

  for (int i = 0; i < NY_CAMERA_GAZE_POINTS; i++)
    {
      const struct ny_camera_gaze_point_s *p = &points[i];
      double row[] = {p->u, p->v, 1};
      if (!isfinite(p->u) || !isfinite(p->v) || !isfinite(p->x) ||
          !isfinite(p->y) || p->u < 0 || p->u > 1 || p->v < 0 ||
          p->v > 1 || fabs(p->x) > 1 || fabs(p->y) > 1)
        return -ERANGE;
      min_u = fmin(min_u, p->u);
      min_v = fmin(min_v, p->v);
      max_u = fmax(max_u, p->u);
      max_v = fmax(max_v, p->v);
      min_x = fmin(min_x, p->x);
      min_y = fmin(min_y, p->y);
      max_x = fmax(max_x, p->x);
      max_y = fmax(max_y, p->y);
      for (int j = 0; j < i; j++)
        if (hypot(p->u - points[j].u, p->v - points[j].v) < 0.01)
          return -EINVAL;
      for (int r = 0; r < 3; r++)
        {
          for (int c = 0; c < 3; c++)
            a[r][c] += row[r] * row[c];
          a[r][3] += row[r] * p->x;
          a[r][4] += row[r] * p->y;
        }
    }

  if (max_u - min_u < NY_CAMERA_GAZE_SPAN ||
      max_v - min_v < NY_CAMERA_GAZE_SPAN ||
      max_x - min_x < NY_CAMERA_GAZE_SPAN ||
      max_y - min_y < NY_CAMERA_GAZE_SPAN)
    return -ERANGE;

  /* Solve both least-squares outputs together, with partial pivoting.
   * The centre sample supplies a consistency check rather than a fifth
   * unconstrained parameter that would conceal calibration error.
   */

  for (int col = 0; col < 3; col++)
    {
      int pivot = col;
      for (int r = col + 1; r < 3; r++)
        if (fabs(a[r][col]) > fabs(a[pivot][col]))
          pivot = r;
      if (fabs(a[pivot][col]) < NY_CAMERA_GAZE_PIVOT)
        return -EINVAL;
      for (int c = col; c < 5; c++)
        {
          double tmp = a[col][c];
          a[col][c] = a[pivot][c];
          a[pivot][c] = tmp;
        }
      double divisor = a[col][col];
      for (int c = col; c < 5; c++)
        a[col][c] /= divisor;
      for (int r = 0; r < 3; r++)
        if (r != col)
          {
            double factor = a[r][col];
            for (int c = col; c < 5; c++)
              a[r][c] -= factor * a[col][c];
          }
    }
  for (int i = 0; i < 3; i++)
    {
      candidate.x[i] = a[i][3];
      candidate.y[i] = a[i][4];
    }
  for (int i = 0; i < NY_CAMERA_GAZE_POINTS; i++)
    {
      const struct ny_camera_gaze_point_s *p = &points[i];
      double dx = candidate.x[0] * p->u + candidate.x[1] * p->v +
                  candidate.x[2] - p->x;
      double dy = candidate.y[0] * p->u + candidate.y[1] * p->v +
                  candidate.y[2] - p->y;
      double error = hypot(dx, dy);
      candidate.rms += error * error;
      candidate.maximum = fmax(candidate.maximum, error);
    }
  candidate.rms = sqrt(candidate.rms / NY_CAMERA_GAZE_POINTS);
  *mapping = candidate;
  return 0;
}

/****************************************************************************
 * Name: ny_camera_gaze_map
 ****************************************************************************/

int ny_camera_gaze_map(const struct ny_camera_gaze_s *mapping,
                       double u, double v, double *x, double *y)
{
  if (mapping == NULL || x == NULL || y == NULL)
    return -EINVAL;
  if (!isfinite(u) || !isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1)
    return -ERANGE;
  double mapped_x = mapping->x[0] * u + mapping->x[1] * v + mapping->x[2];
  double mapped_y = mapping->y[0] * u + mapping->y[1] * v + mapping->y[2];
  if (!isfinite(mapped_x) || !isfinite(mapped_y))
    return -ERANGE;
  *x = ny_camera_gaze_clamp(mapped_x);
  *y = ny_camera_gaze_clamp(mapped_y);
  return 0;
}
