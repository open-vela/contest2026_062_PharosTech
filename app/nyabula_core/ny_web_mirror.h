/****************************************************************************
 * app/nyabula_core/ny_web_mirror.h
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

#ifndef __NYABULA_CORE_NY_WEB_MIRROR_H
#define __NYABULA_CORE_NY_WEB_MIRROR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Is this request for the eye mirror?  `head` is the request head. */

bool ny_web_mirror_claims(const char *head);

/* GET /eyes/stream[?scale=1|2][&fps=1..20]: the two eye pages as a stream
 * of the records nyabula_eye_mirror.h describes, for as long as the
 * connection lasts.  The credentials are those of the socket, as a Bearer
 * token.
 */

int ny_web_mirror_serve(int fd, const char *head, const char *pair_token);

#endif /* __NYABULA_CORE_NY_WEB_MIRROR_H */
