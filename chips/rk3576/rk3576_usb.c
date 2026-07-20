/****************************************************************************
 * chips/rk3576/rk3576_usb.c
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
 * RK3576 USB OTG0 controller driver (Synopsys DesignWare USB3 / DWC3 at
 * 0x23000000).  This file implements the device (peripheral/gadget) role;
 * the host (xHCI) role can be added alongside in a separate file.
 *
 * Scope of the device role:
 *   - USB 2.0 high-speed only.  The USB3 PIPE PHY is left suspended and
 *     the device speed is capped at high speed, which is sufficient for
 *     the CDC/ACM and ADB gadget use cases and avoids bringing up the
 *     USBDP combo PHY.
 *   - One control endpoint pair plus the bulk/interrupt endpoints exposed
 *     through the standard NuttX usbdev endpoint API.  One transfer
 *     request is active per endpoint at a time; further requests queue.
 *
 * Programming model implemented from the public Synopsys DWC3 databook
 * description (global/device register blocks, DEPCMD endpoint commands,
 * TRB descriptors and the single interrupt event buffer).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/queue.h>
#include <nuttx/spinlock.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/usbdev_trace.h>

#include <nuttx/nuttx.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_usb.h"
#include "rk3576_usb.h"

#ifdef CONFIG_RK3576_USB

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Driver geometry ********************************************************/

#define RK3576_NPHYEPS        8 /* ep0out/in + 3 bulk pairs */
#define RK3576_NLOGEPS        (RK3576_NPHYEPS / 2)
#define RK3576_EVTBUF_SIZE    4096
#define RK3576_EP0_MAXPACKET  64
#define RK3576_BULK_MAXPACKET 512
#define RK3576_EP0_BUFSIZE    512

/* EP0 control state */

enum rk3576_ep0state_e
{
  EP0_IDLE = 0,    /* SETUP TRB armed, waiting for a setup packet */
  EP0_SETUP,       /* Setup received, dispatched to the class */
  EP0_DATA_IN,     /* IN data stage in flight */
  EP0_DATA_OUT,    /* OUT data stage in flight */
  EP0_STATUS_WAIT, /* Waiting for XferNotReady(status) */
  EP0_STATUS,      /* Status TRB in flight */
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One DWC3 transfer request block (matches the hardware layout) */

struct rk3576_trb_s
{
  uint32_t bpl;
  uint32_t bph;
  uint32_t size;
  uint32_t ctrl;
};

struct rk3576_req_s
{
  struct usbdev_req_s req; /* Must be first */
  struct sq_entry_s node;
};

/* The queue links through the node member, NOT the struct start: every
 * dequeue must go through this, never a direct cast.
 */

static inline struct rk3576_req_s *rk3576_req_from_node(struct sq_entry_s *e)
{
  return (e == NULL) ? NULL : container_of(e, struct rk3576_req_s, node);
}

struct rk3576_usb_s;

struct rk3576_ep_s
{
  struct usbdev_ep_s ep; /* Must be first */
  struct rk3576_usb_s *dev;
  sq_queue_t reqq;
  struct rk3576_trb_s *trb;
  uint8_t phyep;
  uint8_t rscidx;
  bool enabled;
  bool stalled;
  bool busy; /* A TRB is owned by hardware */
};

struct rk3576_usb_s
{
  struct usbdev_s usbdev; /* Must be first */
  struct usbdevclass_driver_s *driver;

  struct rk3576_ep_s eps[RK3576_NPHYEPS];

  uint32_t *evtbuf;
  uint32_t evtoff;

  /* EP0 machinery */

  enum rk3576_ep0state_e ep0state;
  struct usb_ctrlreq_s ctrlreq;
  uint8_t *setupbuf;  /* 8-byte setup bounce, aligned */
  uint8_t *ep0buf;    /* data stage bounce, aligned */
  uint16_t ep0datlen; /* pending IN data length */
  bool ep0threeStage;
  bool ep0nrdy; /* XferNotReady(data) seen */
  bool ep0pend; /* data TRB waiting for nrdy */

  bool attached;
  uint8_t devaddr;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register access and endpoint commands */

static uint32_t rk3576_usb_getreg(uint32_t offset);
static void rk3576_usb_putreg(uint32_t val, uint32_t offset);
static int rk3576_usb_epcmd(uint8_t phyep, uint32_t cmd, uint32_t p0,
                            uint32_t p1, uint32_t p2, uint32_t *rsc);

/* Core bring-up */

static int rk3576_usb_coreinit(struct rk3576_usb_s *priv);
static void rk3576_usb_ep0out_arm_setup(struct rk3576_usb_s *priv);
static int rk3576_usb_epconfig(struct rk3576_ep_s *privep, uint8_t eptype,
                               uint16_t maxpacket, bool modify);

/* Transfer engine */

static int rk3576_usb_starttrb(struct rk3576_ep_s *privep, void *buf,
                               uint32_t len, uint32_t trbtype);
static void rk3576_usb_reqcomplete(struct rk3576_ep_s *privep, int16_t result);
static void rk3576_usb_epnext(struct rk3576_ep_s *privep);

/* Event handling */

static void rk3576_usb_ep0setup(struct rk3576_usb_s *priv);
static void rk3576_usb_ep0event(struct rk3576_usb_s *priv, uint32_t evt);
static void rk3576_usb_epevent(struct rk3576_usb_s *priv, uint32_t evt);
static void rk3576_usb_devevent(struct rk3576_usb_s *priv, uint32_t evt);
static int rk3576_usb_interrupt(int irq, void *context, void *arg);

/* Endpoint operations */

static int rk3576_ep_configure(struct usbdev_ep_s *ep,
                               const struct usb_epdesc_s *desc, bool last);
static int rk3576_ep_disable(struct usbdev_ep_s *ep);
static struct usbdev_req_s *rk3576_ep_allocreq(struct usbdev_ep_s *ep);
static void rk3576_ep_freereq(struct usbdev_ep_s *ep,
                              struct usbdev_req_s *req);
static void *rk3576_ep_allocbuffer(struct usbdev_ep_s *ep, uint16_t nbytes);
static void rk3576_ep_freebuffer(struct usbdev_ep_s *ep, void *buf);
static int rk3576_ep_submit(struct usbdev_ep_s *ep, struct usbdev_req_s *req);
static int rk3576_ep_cancel(struct usbdev_ep_s *ep, struct usbdev_req_s *req);
static int rk3576_ep_stall(struct usbdev_ep_s *ep, bool resume);

/* Device operations */

static struct usbdev_ep_s *rk3576_dev_allocep(struct usbdev_s *dev,
                                              uint8_t epno, bool in,
                                              uint8_t eptype);
static void rk3576_dev_freeep(struct usbdev_s *dev, struct usbdev_ep_s *ep);
static int rk3576_dev_getframe(struct usbdev_s *dev);
static int rk3576_dev_wakeup(struct usbdev_s *dev);
static int rk3576_dev_selfpowered(struct usbdev_s *dev, bool selfpowered);
static int rk3576_dev_pullup(struct usbdev_s *dev, bool enable);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct usbdev_epops_s g_epops = {
  .configure = rk3576_ep_configure,
  .disable = rk3576_ep_disable,
  .allocreq = rk3576_ep_allocreq,
  .freereq = rk3576_ep_freereq,
  .allocbuffer = rk3576_ep_allocbuffer,
  .freebuffer = rk3576_ep_freebuffer,
  .submit = rk3576_ep_submit,
  .cancel = rk3576_ep_cancel,
  .stall = rk3576_ep_stall,
};

static const struct usbdev_ops_s g_devops = {
  .allocep = rk3576_dev_allocep,
  .freeep = rk3576_dev_freeep,
  .getframe = rk3576_dev_getframe,
  .wakeup = rk3576_dev_wakeup,
  .selfpowered = rk3576_dev_selfpowered,
  .pullup = rk3576_dev_pullup,
};

static struct rk3576_usb_s g_usbdev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_usb_getreg / rk3576_usb_putreg
 ****************************************************************************/

static uint32_t rk3576_usb_getreg(uint32_t offset)
{
  return getreg32(RK3576_USB0_ADDR + offset);
}

static void rk3576_usb_putreg(uint32_t val, uint32_t offset)
{
  putreg32(val, RK3576_USB0_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_usb_epcmd
 *
 * Description:
 *   Issue a DEPCMD endpoint command on a physical endpoint and busy-wait
 *   for completion.  On success, if rsc is non-NULL, return the transfer
 *   resource index reported in the command parameter field (valid after
 *   a Start Transfer command).
 *
 ****************************************************************************/

static int rk3576_usb_epcmd(uint8_t phyep, uint32_t cmd, uint32_t p0,
                            uint32_t p1, uint32_t p2, uint32_t *rsc)
{
  uint32_t reg;
  int retries = 5000;

  rk3576_usb_putreg(p0, DWC3_DEPCMDPAR0(phyep));
  rk3576_usb_putreg(p1, DWC3_DEPCMDPAR1(phyep));
  rk3576_usb_putreg(p2, DWC3_DEPCMDPAR2(phyep));
  rk3576_usb_putreg(cmd | DEPCMD_CMDACT, DWC3_DEPCMD(phyep));

  do
    {
      reg = rk3576_usb_getreg(DWC3_DEPCMD(phyep));
      if ((reg & DEPCMD_CMDACT) == 0)
        {
          break;
        }

      up_udelay(1);
    }
  while (--retries > 0);

  if (retries <= 0)
    {
      uerr("epcmd %lu on phyep %u timeout (GSTS=%08lx DSTS=%08lx "
           "GCTL=%08lx)\n",
           (unsigned long)(cmd & 0xf), phyep,
           (unsigned long)rk3576_usb_getreg(DWC3_GSTS),
           (unsigned long)rk3576_usb_getreg(DWC3_DSTS),
           (unsigned long)rk3576_usb_getreg(DWC3_GCTL));
      return -ETIMEDOUT;
    }

  if ((reg & DEPCMD_STATUS_MASK) != 0)
    {
      uerr("epcmd %lu on phyep %u status %lu\n", (unsigned long)(cmd & 0xf),
           phyep, (unsigned long)((reg & DEPCMD_STATUS_MASK) >> 12));
      return -EIO;
    }

  if (rsc != NULL)
    {
      *rsc = (reg & DEPCMD_PARAM_MASK) >> DEPCMD_PARAM_SHIFT;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_usb_epconfig
 *
 * Description:
 *   Issue DEPCFG + DEPXFERCFG for one physical endpoint.
 *
 ****************************************************************************/

static int rk3576_usb_epconfig(struct rk3576_ep_s *privep, uint8_t eptype,
                               uint16_t maxpacket, bool modify)
{
  uint32_t p0;
  uint32_t p1;
  int ret;

  p0 = (modify ? DEPCFG_P0_ACTION_MODIFY : DEPCFG_P0_ACTION_INIT) |
       DEPCFG_P0_TYPE(eptype) | DEPCFG_P0_MPS(maxpacket);

  /* IN endpoints get a dedicated TX FIFO matching their physical number */

  if ((privep->phyep & 1) != 0)
    {
      p0 |= DEPCFG_P0_FIFONUM(privep->phyep >> 1);
    }

  p1 = DEPCFG_P1_XFERCMPLEN | DEPCFG_P1_XFERNRDYEN |
       DEPCFG_P1_EPNUM(privep->phyep);

  ret = rk3576_usb_epcmd(privep->phyep, DEPCMD_SETEPCONFIG, p0, p1, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_usb_epcmd(privep->phyep, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0,
                          NULL);
}

/****************************************************************************
 * Name: rk3576_usb_starttrb
 *
 * Description:
 *   Program the endpoint's single TRB and issue Start Transfer.  buf must
 *   be cache-line aligned for DMA.
 *
 ****************************************************************************/

static int rk3576_usb_starttrb(struct rk3576_ep_s *privep, void *buf,
                               uint32_t len, uint32_t trbtype)
{
  struct rk3576_trb_s *trb = privep->trb;
  uintptr_t pa = (uintptr_t)buf;
  uint32_t rsc = 0;
  int ret;

  trb->bpl = (uint32_t)(pa & 0xffffffffu);
  trb->bph = (uint32_t)(pa >> 32);
  trb->size = len & TRB_SIZE_MASK;
  trb->ctrl = TRB_CTRL_TYPE(trbtype) | TRB_CTRL_LST | TRB_CTRL_IOC |
              TRB_CTRL_ISP_IMI | TRB_CTRL_HWO;

  up_clean_dcache((uintptr_t)trb,
                  (uintptr_t)trb + sizeof(struct rk3576_trb_s));

  ret = rk3576_usb_epcmd(privep->phyep, DEPCMD_STARTTRANSFER,
                         (uint32_t)((uintptr_t)trb >> 32),
                         (uint32_t)((uintptr_t)trb & 0xffffffffu), 0, &rsc);
  if (ret == OK)
    {
      privep->rscidx = (uint8_t)rsc;
      privep->busy = true;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_usb_ep0out_arm_setup
 *
 * Description:
 *   Arm a Control-Setup TRB on physical endpoint 0 so the next SETUP
 *   packet from the host lands in setupbuf.
 *
 ****************************************************************************/

static void rk3576_usb_ep0out_arm_setup(struct rk3576_usb_s *priv)
{
  up_invalidate_dcache((uintptr_t)priv->setupbuf,
                       (uintptr_t)priv->setupbuf + 64);
  rk3576_usb_starttrb(&priv->eps[0], priv->setupbuf, 8, TRB_TYPE_CTL_SETUP);
  priv->ep0state = EP0_IDLE;
}

/****************************************************************************
 * Name: rk3576_usb_reqcomplete
 *
 * Description:
 *   Complete the request at the head of the endpoint queue and call the
 *   class callback.
 *
 ****************************************************************************/

static void rk3576_usb_reqcomplete(struct rk3576_ep_s *privep, int16_t result)
{
  struct rk3576_req_s *privreq;
  irqstate_t flags;

  flags = enter_critical_section();
  privreq = rk3576_req_from_node(sq_remfirst(&privep->reqq));
  leave_critical_section(flags);

  if (privreq != NULL)
    {
      privreq->req.result = result;
      if (privreq->req.callback != NULL)
        {
          privreq->req.callback(&privep->ep, &privreq->req);
        }
    }
}

/****************************************************************************
 * Name: rk3576_usb_epnext
 *
 * Description:
 *   If the endpoint is idle and a request is queued, start it.
 *
 ****************************************************************************/

static void rk3576_usb_epnext(struct rk3576_ep_s *privep)
{
  struct rk3576_req_s *privreq;
  bool in = (privep->phyep & 1) != 0;

  if (privep->busy || privep->stalled)
    {
      return;
    }

  privreq = rk3576_req_from_node(sq_peek(&privep->reqq));
  if (privreq == NULL)
    {
      return;
    }

  if (privreq->req.buf == NULL)
    {
      rk3576_usb_reqcomplete(privep, -EINVAL);
      return;
    }

  if (in)
    {
      up_clean_dcache((uintptr_t)privreq->req.buf,
                      (uintptr_t)privreq->req.buf + privreq->req.len);
      rk3576_usb_starttrb(privep, privreq->req.buf, privreq->req.len,
                          TRB_TYPE_NORMAL);
    }
  else
    {
      up_invalidate_dcache((uintptr_t)privreq->req.buf,
                           (uintptr_t)privreq->req.buf + privreq->req.len);
      rk3576_usb_starttrb(privep, privreq->req.buf, privreq->req.len,
                          TRB_TYPE_NORMAL);
    }
}

/****************************************************************************
 * Name: rk3576_usb_ep0setup
 *
 * Description:
 *   A SETUP packet arrived in setupbuf: handle SET_ADDRESS locally and
 *   forward everything else to the class driver.
 *
 ****************************************************************************/

static void rk3576_usb_ep0setup(struct rk3576_usb_s *priv)
{
  struct usb_ctrlreq_s *ctrl = &priv->ctrlreq;
  uint16_t value;
  uint16_t len;
  int ret;

  up_invalidate_dcache((uintptr_t)priv->setupbuf,
                       (uintptr_t)priv->setupbuf + 64);
  memcpy(ctrl, priv->setupbuf, sizeof(struct usb_ctrlreq_s));

  value = GETUINT16(ctrl->value);
  len = GETUINT16(ctrl->len);

  uinfo("SETUP type=%02x req=%02x val=%04x len=%u\n", ctrl->type, ctrl->req,
        value, len);

  priv->ep0threeStage = (len != 0);
  priv->ep0state = EP0_SETUP;
  priv->ep0nrdy = false;
  priv->ep0pend = false;

  /* A new SETUP aborts whatever control transfer was in flight: kill a
   * stale ep0 IN data/status TRB or its transfer resource stays busy
   * and the next Start Transfer is rejected.
   */

  if (priv->eps[1].busy)
    {
      rk3576_usb_epcmd(
          1,
          DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_CMDIOC |
              ((uint32_t)priv->eps[1].rscidx << DEPCMD_PARAM_SHIFT),
          0, 0, 0, NULL);
      priv->eps[1].busy = false;
      while (sq_peek(&priv->eps[1].reqq) != NULL)
        {
          rk3576_usb_reqcomplete(&priv->eps[1], -ESHUTDOWN);
        }
    }

  /* SET_ADDRESS is handled entirely in the controller driver */

  if ((ctrl->type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD &&
      ctrl->req == USB_REQ_SETADDRESS)
    {
      uint32_t dcfg = rk3576_usb_getreg(DWC3_DCFG);
      dcfg &= ~DCFG_DEVADDR_MASK;
      dcfg |= ((uint32_t)(value & 0x7f)) << DCFG_DEVADDR_SHIFT;
      rk3576_usb_putreg(dcfg, DWC3_DCFG);
      priv->devaddr = value & 0x7f;
      priv->ep0state = EP0_STATUS_WAIT;
      return;
    }

  /* Forward to the class.  For OUT data stages the class expects the data
   * to accompany the setup, so receive the data stage first.
   */

  if (len != 0 && (ctrl->type & USB_DIR_IN) == 0)
    {
      /* Defer the data TRB to XferNotReady(data): the core rejects a
       * Start Transfer for a control data stage the host has not begun.
       */

      priv->ep0state = EP0_DATA_OUT;
      priv->ep0datlen =
          len < RK3576_EP0_BUFSIZE
              ? RK3576_EP0_MAXPACKET *
                    ((len + RK3576_EP0_MAXPACKET - 1) / RK3576_EP0_MAXPACKET)
              : RK3576_EP0_BUFSIZE;
      priv->ep0pend = true;
      return;
    }

  ret = CLASS_SETUP(priv->driver, &priv->usbdev, ctrl, NULL, 0);
  if (ret < 0)
    {
      uerr("class setup failed %d, stalling ep0\n", ret);
      rk3576_usb_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0, NULL);
      rk3576_usb_ep0out_arm_setup(priv);
      return;
    }

  /* If there is no data stage the class returned OK and we only owe the
   * host a status stage.  If there is an IN data stage the class queued
   * it via EP_SUBMIT on ep0 (handled in rk3576_ep_submit).
   */

  if (len == 0)
    {
      priv->ep0state = EP0_STATUS_WAIT;
    }
}

/****************************************************************************
 * Name: rk3576_usb_ep0event
 *
 * Description:
 *   Endpoint events for physical endpoints 0 (ep0 OUT) and 1 (ep0 IN):
 *   drive the three-phase control state machine.
 *
 ****************************************************************************/

static void rk3576_usb_ep0event(struct rk3576_usb_s *priv, uint32_t evt)
{
  uint8_t phyep = DEPEVT_PHYEP(evt);
  uint8_t type = DEPEVT_TYPE(evt);
  uint8_t status = DEPEVT_STATUS(evt);

  if (type == DEPEVT_XFERCOMPLETE)
    {
      priv->eps[phyep].busy = false;

      switch (priv->ep0state)
        {
          case EP0_IDLE:

            /* SETUP packet landed */

            rk3576_usb_ep0setup(priv);
            break;

          case EP0_DATA_OUT:
            {
              struct usb_ctrlreq_s *ctrl = &priv->ctrlreq;
              uint16_t len = GETUINT16(ctrl->len);

              up_invalidate_dcache((uintptr_t)priv->ep0buf,
                                   (uintptr_t)priv->ep0buf +
                                       RK3576_EP0_BUFSIZE);
              CLASS_SETUP(priv->driver, &priv->usbdev, ctrl, priv->ep0buf,
                          len);
              priv->ep0state = EP0_STATUS_WAIT;
            }
            break;

          case EP0_DATA_IN:
            priv->ep0state = EP0_STATUS_WAIT;
            rk3576_usb_reqcomplete(&priv->eps[1], OK);
            break;

          case EP0_STATUS:

            /* Control transfer done: re-arm for the next SETUP */

            rk3576_usb_ep0out_arm_setup(priv);
            break;

          default:
            rk3576_usb_ep0out_arm_setup(priv);
            break;
        }
    }
  else if (type == DEPEVT_XFERNOTREADY)
    {
      /* Start the status stage when the host asks for it */

      if ((status & 3) == DEPEVT_NRDY_CTL_STATUS &&
          priv->ep0state == EP0_STATUS_WAIT)
        {
          /* Status direction: IN status on phyep1 for OUT/no-data
           * transfers, OUT status on phyep0 for IN data transfers.
           * The not-ready event arrives on the endpoint the host is
           * polling, which is exactly where the status TRB belongs.
           */

          uint32_t trbtype = priv->ep0threeStage ? TRB_TYPE_CTL_STATUS3
                                                 : TRB_TYPE_CTL_STATUS2;
          priv->ep0state = EP0_STATUS;
          rk3576_usb_starttrb(&priv->eps[phyep], priv->setupbuf, 0, trbtype);
        }
      else if ((status & 3) == DEPEVT_NRDY_CTL_DATA)
        {
          /* Host started the data stage: launch the deferred data TRB */

          if (priv->ep0pend && priv->ep0state == EP0_DATA_IN)
            {
              priv->ep0pend = false;
              rk3576_usb_starttrb(&priv->eps[1], priv->ep0buf, priv->ep0datlen,
                                  TRB_TYPE_CTL_DATA);
            }
          else if (priv->ep0pend && priv->ep0state == EP0_DATA_OUT)
            {
              priv->ep0pend = false;
              up_invalidate_dcache((uintptr_t)priv->ep0buf,
                                   (uintptr_t)priv->ep0buf +
                                       RK3576_EP0_BUFSIZE);
              rk3576_usb_starttrb(&priv->eps[0], priv->ep0buf, priv->ep0datlen,
                                  TRB_TYPE_CTL_DATA);
            }
          else
            {
              priv->ep0nrdy = true;
            }
        }
    }
}

/****************************************************************************
 * Name: rk3576_usb_epevent
 *
 * Description:
 *   Endpoint events for the non-control endpoints.
 *
 ****************************************************************************/

static void rk3576_usb_epevent(struct rk3576_usb_s *priv, uint32_t evt)
{
  uint8_t phyep = DEPEVT_PHYEP(evt);
  struct rk3576_ep_s *privep;

  if (phyep < 2)
    {
      rk3576_usb_ep0event(priv, evt);
      return;
    }

  if (phyep >= RK3576_NPHYEPS)
    {
      return;
    }

  privep = &priv->eps[phyep];

  if (DEPEVT_TYPE(evt) == DEPEVT_XFERCOMPLETE)
    {
      struct rk3576_req_s *privreq;

      privep->busy = false;
      privreq = rk3576_req_from_node(sq_peek(&privep->reqq));
      if (privreq != NULL)
        {
          if ((privep->phyep & 1) == 0)
            {
              /* OUT: actual received length = requested - remaining */

              up_invalidate_dcache((uintptr_t)privep->trb,
                                   (uintptr_t)privep->trb +
                                       sizeof(struct rk3576_trb_s));
              up_invalidate_dcache((uintptr_t)privreq->req.buf,
                                   (uintptr_t)privreq->req.buf +
                                       privreq->req.len);
              privreq->req.xfrd =
                  privreq->req.len - (privep->trb->size & TRB_SIZE_MASK);
            }
          else
            {
              privreq->req.xfrd = privreq->req.len;
            }

          rk3576_usb_reqcomplete(privep, OK);
        }

      rk3576_usb_epnext(privep);
    }
}

/****************************************************************************
 * Name: rk3576_usb_devevent
 *
 * Description:
 *   Device-level events: reset, connect done, disconnect, suspend/resume.
 *
 ****************************************************************************/

static void rk3576_usb_devevent(struct rk3576_usb_s *priv, uint32_t evt)
{
  switch (DEVT_TYPE(evt))
    {
      case DEVT_USBRST:
        {
          uint32_t dcfg;
          int i;

          uinfo("USB reset\n");

          /* Drop the device address and flush endpoint state */

          dcfg = rk3576_usb_getreg(DWC3_DCFG);
          dcfg &= ~DCFG_DEVADDR_MASK;
          rk3576_usb_putreg(dcfg, DWC3_DCFG);
          priv->devaddr = 0;

          /* Kill any in-flight ep0 transfer so the SETUP TRB can be
           * re-armed cleanly once ConnectDone arrives.
           */

          for (i = 0; i < 2; i++)
            {
              if (priv->eps[i].busy)
                {
                  rk3576_usb_epcmd((uint8_t)i,
                                   DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM |
                                       DEPCMD_CMDIOC |
                                       ((uint32_t)priv->eps[i].rscidx
                                        << DEPCMD_PARAM_SHIFT),
                                   0, 0, 0, NULL);
                  priv->eps[i].busy = false;
                }
            }

          priv->ep0state = EP0_IDLE;

          for (i = 2; i < RK3576_NPHYEPS; i++)
            {
              struct rk3576_ep_s *privep = &priv->eps[i];

              privep->stalled = false;
              privep->busy = false;
              while (sq_peek(&privep->reqq) != NULL)
                {
                  rk3576_usb_reqcomplete(privep, -ESHUTDOWN);
                }
            }

          if (priv->driver != NULL)
            {
              CLASS_DISCONNECT(priv->driver, &priv->usbdev);
            }
        }
        break;

      case DEVT_CONNECTDONE:
        {
          uint32_t speed = rk3576_usb_getreg(DWC3_DSTS) & DSTS_CONNECTSPD_MASK;
          uint16_t mps = (speed == DSTS_SPEED_FS) ? 64 : 64;

          uinfo("connect done, speed=%lu\n", (unsigned long)speed);

          priv->usbdev.speed =
              (speed == DSTS_SPEED_HS) ? USB_SPEED_HIGH : USB_SPEED_FULL;

          /* Re-issue ep0 config with the final control MPS (modify) */

          rk3576_usb_epconfig(&priv->eps[0], USB_EP_ATTR_XFER_CONTROL, mps,
                              true);
          rk3576_usb_epconfig(&priv->eps[1], USB_EP_ATTR_XFER_CONTROL, mps,
                              true);

          /* (Re-)arm for the first SETUP of the new connection */

          if (!priv->eps[0].busy)
            {
              rk3576_usb_ep0out_arm_setup(priv);
            }
        }
        break;

      case DEVT_DISCONN:
        uinfo("disconnect\n");
        if (priv->driver != NULL)
          {
            CLASS_DISCONNECT(priv->driver, &priv->usbdev);
          }
        break;

      case DEVT_SUSPEND:
        if (priv->driver != NULL && priv->driver->ops->suspend != NULL)
          {
            CLASS_SUSPEND(priv->driver, &priv->usbdev);
          }
        break;

      case DEVT_WKUPEVT:
        if (priv->driver != NULL && priv->driver->ops->resume != NULL)
          {
            CLASS_RESUME(priv->driver, &priv->usbdev);
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: rk3576_usb_interrupt
 *
 * Description:
 *   Drain the event buffer and dispatch each 4-byte event.
 *
 ****************************************************************************/

static int rk3576_usb_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_usb_s *priv = &g_usbdev;
  uint32_t count;
  uint32_t evt;

  count = rk3576_usb_getreg(DWC3_GEVNTCOUNT) & 0xffffu;

  while (count != 0)
    {
      up_invalidate_dcache((uintptr_t)priv->evtbuf,
                           (uintptr_t)priv->evtbuf + RK3576_EVTBUF_SIZE);

      evt = priv->evtbuf[priv->evtoff / 4];
      priv->evtoff = (priv->evtoff + 4) % RK3576_EVTBUF_SIZE;

      if (EVT_IS_DEVT(evt))
        {
          rk3576_usb_devevent(priv, evt);
        }
      else
        {
          rk3576_usb_epevent(priv, evt);
        }

      /* Tell the controller we consumed one event */

      rk3576_usb_putreg(4, DWC3_GEVNTCOUNT);

      count -= 4;
      if (count == 0)
        {
          count = rk3576_usb_getreg(DWC3_GEVNTCOUNT) & 0xffffu;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_usb_coreinit
 *
 * Description:
 *   Reset the DWC3 core, force peripheral mode, cap the speed at USB 2.0
 *   high speed, program the event buffer and start endpoint 0.
 *
 ****************************************************************************/

static int rk3576_usb_coreinit(struct rk3576_usb_s *priv)
{
  uint32_t reg;
  int retries;
  int ret;

  /* Sanity: the Synopsys ID register identifies a USB3 DWC core */

  reg = rk3576_usb_getreg(DWC3_GSNPSID);
  uinfo("GSNPSID=%08lx\n", (unsigned long)reg);
  if ((reg & 0xffff0000u) != 0x55330000u)
    {
      uerr("not a DWC3 core (GSNPSID=%08lx) - clocks off?\n",
           (unsigned long)reg);
      return -ENODEV;
    }

  /* USB2 PHY0 GRF: force the port to normal (not suspended) operation.
   * con0 low bits are the software override controls; the hiword-masked
   * write clears them all so the PHY free-runs and supplies the UTMI
   * clock regardless of what the boot loader left behind.
   */

  putreg32(0x01ff0000u, 0x2602e000ul);
  up_mdelay(2);

  /* USB_GRF USB3OTG0_CON1: USB2-only operation without the USBDP combo
   * PHY.  pipe_clk_sel=1 substitutes the UTMI clock for the (dead) PIPE
   * clock -- without this the endpoint command engine wedges on the
   * first DEPCFG -- and phystatus_con=2'b10 forces PIPE phystatus to 0
   * so the core does not wait on the missing PHY.  (TRM part1 5.14,
   * board-verified 2026-07-14.)
   */

  putreg32(0x008c0088u, 0x2601e030ul);

  /* Full core soft reset with both PHY interfaces held in reset while
   * the core resets: cleans up whatever mode/state the boot loader left
   * the dual-role core in.
   */

  reg = rk3576_usb_getreg(DWC3_GCTL);
  reg |= GCTL_CORESOFTRESET;
  rk3576_usb_putreg(reg, DWC3_GCTL);

  reg = rk3576_usb_getreg(DWC3_GUSB2PHYCFG);
  rk3576_usb_putreg(reg | GUSB2PHYCFG_PHYSOFTRST, DWC3_GUSB2PHYCFG);
  reg = rk3576_usb_getreg(DWC3_GUSB3PIPECTL);
  rk3576_usb_putreg(reg | GUSB3PIPECTL_PHYSOFTRST, DWC3_GUSB3PIPECTL);
  up_mdelay(1);

  reg = rk3576_usb_getreg(DWC3_GUSB2PHYCFG);
  rk3576_usb_putreg(reg & ~GUSB2PHYCFG_PHYSOFTRST, DWC3_GUSB2PHYCFG);
  reg = rk3576_usb_getreg(DWC3_GUSB3PIPECTL);
  rk3576_usb_putreg(reg & ~GUSB3PIPECTL_PHYSOFTRST, DWC3_GUSB3PIPECTL);
  up_mdelay(1);

  reg = rk3576_usb_getreg(DWC3_GCTL);
  reg &= ~GCTL_CORESOFTRESET;
  rk3576_usb_putreg(reg, DWC3_GCTL);
  up_mdelay(1);

  /* Force peripheral (device) role: the boot loader may have left the
   * dual-role core in host mode, where the device block (and its
   * command engine) is held inactive.
   */

  reg = rk3576_usb_getreg(DWC3_GCTL);
  reg &= ~(GCTL_PRTCAPDIR_MASK | GCTL_DSBLCLKGTNG);
  reg |= GCTL_PRTCAP_DEVICE;
  rk3576_usb_putreg(reg, DWC3_GCTL);

  /* Device core soft reset */

  rk3576_usb_putreg(DCTL_CSFTRST, DWC3_DCTL);
  retries = 1000;
  do
    {
      reg = rk3576_usb_getreg(DWC3_DCTL);
      if ((reg & DCTL_CSFTRST) == 0)
        {
          break;
        }

      up_mdelay(1);
    }
  while (--retries > 0);

  if (retries <= 0)
    {
      uerr("CSFTRST stuck\n");
      return -ETIMEDOUT;
    }

  /* USB2 PHY interface: 16-bit UTMI+ ("utmi_wide"), no auto-suspend
   * while we bring the link up.
   */

  /* U2_FREECLK_EXISTS must be cleared on this SoC (the vendor device
   * tree carries snps,dis-u2-freeclk-exists-quirk): with it set the
   * core assumes a free-running PHY clock that is not there and the
   * endpoint command engine wedges on the first DEPCFG.
   */

  reg = rk3576_usb_getreg(DWC3_GUSB2PHYCFG);
  reg &= ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM |
           GUSB2PHYCFG_TRDTIM_MASK | GUSB2PHYCFG_U2FREECLK);
  reg |= GUSB2PHYCFG_PHYIF | GUSB2PHYCFG_TRDTIM(5);
  rk3576_usb_putreg(reg, DWC3_GUSB2PHYCFG);

  /* USB3 PIPE PHY: keep the suspend bit CLEAR while initializing.  On
   * this core revision (3.00a) endpoint commands stall if the PIPE
   * clock is gated by the suspend bit.  High-speed only operation is
   * enforced through DCFG.DevSpd instead.
   */

  reg = rk3576_usb_getreg(DWC3_GUSB3PIPECTL);
  reg &= ~(GUSB3PIPECTL_SUSPEND | GUSB3PIPECTL_PHYSOFTRST);
  rk3576_usb_putreg(reg, DWC3_GUSB3PIPECTL);

  /* Event buffer */

  memset(priv->evtbuf, 0, RK3576_EVTBUF_SIZE);
  up_clean_dcache((uintptr_t)priv->evtbuf,
                  (uintptr_t)priv->evtbuf + RK3576_EVTBUF_SIZE);
  priv->evtoff = 0;

  rk3576_usb_putreg((uint32_t)((uintptr_t)priv->evtbuf & 0xffffffffu),
                    DWC3_GEVNTADRLO);
  rk3576_usb_putreg((uint32_t)((uintptr_t)priv->evtbuf >> 32),
                    DWC3_GEVNTADRHI);
  rk3576_usb_putreg(RK3576_EVTBUF_SIZE, DWC3_GEVNTSIZ);
  rk3576_usb_putreg(0, DWC3_GEVNTCOUNT);

  /* Device speed: high speed (USB 2.0) */

  reg = rk3576_usb_getreg(DWC3_DCFG);
  reg &= ~(DCFG_DEVSPD_MASK | DCFG_DEVADDR_MASK);
  reg |= DCFG_DEVSPD_HS;
  rk3576_usb_putreg(reg, DWC3_DCFG);

  /* Start endpoint configuration: DEPSTARTCFG with resource index 0
   * assigns transfer resources for ep0 OUT/IN.
   */

  ret = rk3576_usb_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_usb_epconfig(&priv->eps[0], USB_EP_ATTR_XFER_CONTROL,
                            RK3576_EP0_MAXPACKET, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_usb_epconfig(&priv->eps[1], USB_EP_ATTR_XFER_CONTROL,
                            RK3576_EP0_MAXPACKET, false);
  if (ret < 0)
    {
      return ret;
    }

  /* Enable ep0 OUT/IN and arm for the first SETUP */

  rk3576_usb_putreg(3, DWC3_DALEPENA);
  rk3576_usb_ep0out_arm_setup(priv);

  /* Device event interrupts */

  rk3576_usb_putreg(DEVTEN_DISCONNEVTEN | DEVTEN_USBRSTEN |
                        DEVTEN_CONNECTDONEEN | DEVTEN_WKUPEVTEN |
                        DEVTEN_U3L2L1SUSPEN,
                    DWC3_DEVTEN);

  return OK;
}

/****************************************************************************
 * Endpoint operations
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_ep_configure
 ****************************************************************************/

static int rk3576_ep_configure(struct usbdev_ep_s *ep,
                               const struct usb_epdesc_s *desc, bool last)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;
  uint16_t maxpacket = GETUINT16(desc->mxpacketsize);
  uint8_t eptype = desc->attr & USB_EP_ATTR_XFERTYPE_MASK;
  uint32_t reg;
  int ret;

  /* No DEPSTARTCFG here: on this core revision (3.00a) transfer
   * resources are pre-allocated and Start New Configuration is issued
   * only once, with resource index 0, at controller initialization.
   * Each endpoint still gets its own DEPXFERCFG (in epconfig below).
   */

  ret = rk3576_usb_epconfig(privep, eptype, maxpacket, false);
  if (ret < 0)
    {
      return ret;
    }

  ep->maxpacket = maxpacket;
  privep->enabled = true;
  privep->stalled = false;

  reg = rk3576_usb_getreg(DWC3_DALEPENA);
  reg |= (1u << privep->phyep);
  rk3576_usb_putreg(reg, DWC3_DALEPENA);

  return OK;
}

/****************************************************************************
 * Name: rk3576_ep_disable
 ****************************************************************************/

static int rk3576_ep_disable(struct usbdev_ep_s *ep)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;
  uint32_t reg;
  irqstate_t flags;

  flags = enter_critical_section();

  if (privep->busy)
    {
      rk3576_usb_epcmd(privep->phyep,
                       DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM |
                           DEPCMD_CMDIOC |
                           ((uint32_t)privep->rscidx << DEPCMD_PARAM_SHIFT),
                       0, 0, 0, NULL);
      privep->busy = false;
    }

  reg = rk3576_usb_getreg(DWC3_DALEPENA);
  reg &= ~(1u << privep->phyep);
  rk3576_usb_putreg(reg, DWC3_DALEPENA);

  privep->enabled = false;

  while (sq_peek(&privep->reqq) != NULL)
    {
      rk3576_usb_reqcomplete(privep, -ESHUTDOWN);
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_ep_allocreq / rk3576_ep_freereq
 ****************************************************************************/

static struct usbdev_req_s *rk3576_ep_allocreq(struct usbdev_ep_s *ep)
{
  struct rk3576_req_s *privreq;

  privreq = kmm_zalloc(sizeof(struct rk3576_req_s));
  if (privreq == NULL)
    {
      return NULL;
    }

  return &privreq->req;
}

static void rk3576_ep_freereq(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  kmm_free(req);
}

/****************************************************************************
 * Name: rk3576_ep_allocbuffer / rk3576_ep_freebuffer
 *
 * Description:
 *   Endpoint DMA buffers must be cache-line aligned; hand out aligned
 *   allocations so requests can be DMAed in place.
 *
 ****************************************************************************/

static void *rk3576_ep_allocbuffer(struct usbdev_ep_s *ep, uint16_t nbytes)
{
  return kmm_memalign(64, (nbytes + 63) & ~63u);
}

static void rk3576_ep_freebuffer(struct usbdev_ep_s *ep, void *buf)
{
  kmm_free(buf);
}

/****************************************************************************
 * Name: rk3576_ep_submit
 ****************************************************************************/

static int rk3576_ep_submit(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;
  struct rk3576_usb_s *priv = privep->dev;
  struct rk3576_req_s *privreq = (struct rk3576_req_s *)req;
  irqstate_t flags;

  req->result = -EINPROGRESS;
  req->xfrd = 0;

  flags = enter_critical_section();

  /* EP0 zero-length submission: the class's status-phase acknowledge.
   * The status stage itself is driven by the state machine (STATUS2/3
   * on XferNotReady), so just complete the request -- starting a TRB
   * here would occupy the ep0 transfer resource and poison the next
   * control transfer.
   */

  if (privep->phyep < 2 && req->len == 0)
    {
      req->result = OK;
      req->xfrd = 0;
      leave_critical_section(flags);
      if (req->callback != NULL)
        {
          req->callback(&privep->ep, req);
        }

      return OK;
    }

  /* EP0 IN: this is the data stage answer to the last SETUP */

  if (privep->phyep == 1 && priv->ep0state == EP0_SETUP)
    {
      uint16_t len = req->len;

      if (len > RK3576_EP0_BUFSIZE)
        {
          len = RK3576_EP0_BUFSIZE;
        }

      memcpy(priv->ep0buf, req->buf, len);
      up_clean_dcache((uintptr_t)priv->ep0buf,
                      (uintptr_t)priv->ep0buf + RK3576_EP0_BUFSIZE);

      sq_addlast(&privreq->node, &privep->reqq);
      priv->ep0state = EP0_DATA_IN;
      priv->ep0datlen = len;

      /* Start the data TRB only once the host has begun the data stage
       * (XferNotReady(data)); starting early is rejected by the core.
       */

      if (priv->ep0nrdy)
        {
          priv->ep0nrdy = false;
          rk3576_usb_starttrb(privep, priv->ep0buf, len, TRB_TYPE_CTL_DATA);
        }
      else
        {
          priv->ep0pend = true;
        }

      leave_critical_section(flags);
      return OK;
    }

  sq_addlast(&privreq->node, &privep->reqq);
  rk3576_usb_epnext(privep);

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_ep_cancel
 ****************************************************************************/

static int rk3576_ep_cancel(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;
  irqstate_t flags;

  flags = enter_critical_section();

  if (privep->busy)
    {
      rk3576_usb_epcmd(privep->phyep,
                       DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM |
                           DEPCMD_CMDIOC |
                           ((uint32_t)privep->rscidx << DEPCMD_PARAM_SHIFT),
                       0, 0, 0, NULL);
      privep->busy = false;
    }

  while (sq_peek(&privep->reqq) != NULL)
    {
      rk3576_usb_reqcomplete(privep, -ESHUTDOWN);
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_ep_stall
 ****************************************************************************/

static int rk3576_ep_stall(struct usbdev_ep_s *ep, bool resume)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;
  int ret;

  if (resume)
    {
      ret = rk3576_usb_epcmd(privep->phyep, DEPCMD_CLEARSTALL, 0, 0, 0, NULL);
      privep->stalled = false;
      rk3576_usb_epnext(privep);
    }
  else
    {
      ret = rk3576_usb_epcmd(privep->phyep, DEPCMD_SETSTALL, 0, 0, 0, NULL);
      privep->stalled = true;
    }

  return ret;
}

/****************************************************************************
 * Device operations
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dev_allocep
 ****************************************************************************/

static struct usbdev_ep_s *
rk3576_dev_allocep(struct usbdev_s *dev, uint8_t epno, bool in, uint8_t eptype)
{
  struct rk3576_usb_s *priv = (struct rk3576_usb_s *)dev;
  uint8_t log = epno & USB_EPNO_MASK;
  int i;

  /* Specific endpoint requested */

  if (log != 0)
    {
      uint8_t phy = (uint8_t)((log << 1) | (in ? 1 : 0));

      if (phy < RK3576_NPHYEPS && !priv->eps[phy].enabled)
        {
          return &priv->eps[phy].ep;
        }

      return NULL;
    }

  /* Any endpoint of the right direction */

  for (i = 2; i < RK3576_NPHYEPS; i++)
    {
      if (((i & 1) != 0) == in && !priv->eps[i].enabled &&
          priv->eps[i].ep.eplog == 0)
        {
          return &priv->eps[i].ep;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: rk3576_dev_freeep
 ****************************************************************************/

static void rk3576_dev_freeep(struct usbdev_s *dev, struct usbdev_ep_s *ep)
{
  struct rk3576_ep_s *privep = (struct rk3576_ep_s *)ep;

  if (privep->enabled)
    {
      rk3576_ep_disable(ep);
    }
}

/****************************************************************************
 * Name: rk3576_dev_getframe / wakeup / selfpowered / pullup
 ****************************************************************************/

static int rk3576_dev_getframe(struct usbdev_s *dev)
{
  return (int)((rk3576_usb_getreg(DWC3_DSTS) >> 3) & 0x3fffu);
}

static int rk3576_dev_wakeup(struct usbdev_s *dev) { return -ENOSYS; }

static int rk3576_dev_selfpowered(struct usbdev_s *dev, bool selfpowered)
{
  return OK;
}

/****************************************************************************
 * Name: rk3576_dev_pullup
 *
 * Description:
 *   Connect/disconnect from the bus by setting/clearing DCTL.RunStop.
 *
 ****************************************************************************/

static int rk3576_dev_pullup(struct usbdev_s *dev, bool enable)
{
  uint32_t reg;

  reg = rk3576_usb_getreg(DWC3_DCTL);
  if (enable)
    {
      reg |= DCTL_RUNSTOP;
    }
  else
    {
      reg &= ~DCTL_RUNSTOP;
    }

  rk3576_usb_putreg(reg, DWC3_DCTL);
  uinfo("pullup %d\n", enable);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_usb_initialize
 ****************************************************************************/

int rk3576_usb_initialize(void)
{
  struct rk3576_usb_s *priv = &g_usbdev;
  struct rk3576_trb_s *trbs;
  int i;

  if (priv->evtbuf != NULL)
    {
      return OK; /* Already initialized */
    }

  memset(priv, 0, sizeof(*priv));

  priv->usbdev.ops = &g_devops;
  priv->usbdev.ep0 = &priv->eps[1].ep;
  priv->usbdev.speed = USB_SPEED_HIGH;
  priv->usbdev.dualspeed = 1;

  /* DMA-able state: event buffer, one TRB per endpoint, EP0 bounce
   * buffers.  Cache-line (64) aligned; the TRB array is per-TRB aligned
   * (16 bytes required, 64 used so each TRB owns its cache line).
   */

  priv->evtbuf = kmm_memalign(4096, RK3576_EVTBUF_SIZE);
  trbs = kmm_memalign(64, RK3576_NPHYEPS * 64);
  priv->setupbuf = kmm_memalign(64, 64);
  priv->ep0buf = kmm_memalign(64, RK3576_EP0_BUFSIZE);

  if (priv->evtbuf == NULL || trbs == NULL || priv->setupbuf == NULL ||
      priv->ep0buf == NULL)
    {
      return -ENOMEM;
    }

  memset(trbs, 0, RK3576_NPHYEPS * 64);

  for (i = 0; i < RK3576_NPHYEPS; i++)
    {
      struct rk3576_ep_s *privep = &priv->eps[i];

      privep->ep.ops = &g_epops;
      privep->ep.eplog = (uint8_t)((i >> 1) | ((i & 1) ? USB_DIR_IN : 0));
      privep->ep.maxpacket =
          (i < 2) ? RK3576_EP0_MAXPACKET : RK3576_BULK_MAXPACKET;
      privep->dev = priv;
      privep->phyep = (uint8_t)i;
      privep->trb = (struct rk3576_trb_s *)((uintptr_t)trbs + i * 64);
      sq_init(&privep->reqq);
    }

  irq_attach(RK3576_IRQ_USB0, rk3576_usb_interrupt, NULL);
  up_enable_irq(RK3576_IRQ_USB0);

  return rk3576_usb_coreinit(priv);
}

/****************************************************************************
 * Name: arm64_usbinitialize / arm64_usbuninitialize
 *
 * Description:
 *   Architecture hooks referenced by up_initialize() when CONFIG_USBDEV is
 *   selected.  Full controller initialization is deferred to the first
 *   usbdev_register() so a class-less configuration boots cleanly.
 *
 ****************************************************************************/

void arm64_usbinitialize(void) {}

void arm64_usbuninitialize(void) {}

/****************************************************************************
 * Name: usbdev_register
 *
 * Description:
 *   NuttX USB device stack entry point: bind a class driver to this
 *   controller and go on the bus.
 *
 ****************************************************************************/

int usbdev_register(struct usbdevclass_driver_s *driver)
{
  struct rk3576_usb_s *priv = &g_usbdev;
  int ret;

  if (driver == NULL || driver->ops == NULL || driver->ops->bind == NULL)
    {
      return -EINVAL;
    }

  if (priv->driver != NULL)
    {
      return -EBUSY;
    }

  ret = rk3576_usb_initialize();
  if (ret < 0)
    {
      return ret;
    }

  priv->driver = driver;

  ret = CLASS_BIND(driver, &priv->usbdev);
  if (ret < 0)
    {
      priv->driver = NULL;
      return ret;
    }

  /* Go on the bus */

  rk3576_dev_pullup(&priv->usbdev, true);
  return OK;
}

/****************************************************************************
 * Name: usbdev_unregister
 ****************************************************************************/

int usbdev_unregister(struct usbdevclass_driver_s *driver)
{
  struct rk3576_usb_s *priv = &g_usbdev;

  if (driver != priv->driver)
    {
      return -EINVAL;
    }

  rk3576_dev_pullup(&priv->usbdev, false);
  CLASS_UNBIND(driver, &priv->usbdev);
  priv->driver = NULL;
  return OK;
}

#endif /* CONFIG_RK3576_USB */
