/****************************************************************************
 * tools/nyabula_core/tests/cmake/wamr_config.c
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

#if WASM_ENABLE_MODULE_INST_CONTEXT != NYCORE_TEST_EXPECTED_CONTEXT
#error Core must preserve the WAMR-owned module context setting
#endif

int nycore_test_wamr_configuration(void)
{
  return WASM_ENABLE_MODULE_INST_CONTEXT;
}
