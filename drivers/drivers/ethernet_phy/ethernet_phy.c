/****************************************************************************
 * drivers/drivers/ethernet_phy/ethernet_phy.c
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

/****************************************************************************
 * The two entry points a MAC driver's configuration refers to.
 *
 * They exist so that a MAC driver can hold plain function pointers without
 * including the PHY interface header: the board puts these in its
 * configuration and passes its own `struct ethernet_phy_s` as the context, and
 * the dispatch to the part's ops happens here.  Adding a second PHY part costs
 * one more ops table; the board still only wires two pointers.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>

#include <ethernet_phy.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ethernet_phy_init(FAR void *ctx)
{
  FAR struct ethernet_phy_s *phy = ctx;

  if (phy == NULL || phy->ops == NULL || phy->ops->init == NULL)
    {
      /* No part-specific configuration to apply; the port is still usable as
       * a clause 22 port.
       */

      return 0;
    }

  return phy->ops->init(phy);
}

int ethernet_phy_advertise(FAR void *ctx)
{
  FAR struct ethernet_phy_s *phy = ctx;

  if (phy == NULL || phy->ops == NULL || phy->ops->advertise == NULL)
    {
      return 0;
    }

  return phy->ops->advertise(phy);
}
