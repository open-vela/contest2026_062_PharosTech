/***************************************************************************
 * arch/arm64/src/rk3576/rk3576_serial.c
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
 ***************************************************************************/

/* Reference:
 *
 * "NuttX RTOS for PinePhone: UART Driver"
 * https://lupyuen.github.io/articles/serial
 *
 * "A64 Page" refers to Allwinner A64 User Manual
 * https://lupyuen.github.io/images/Allwinner_A64_User_Manual_V1.1.pdf
 */

/***************************************************************************
 * Included Files
 ***************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#ifdef CONFIG_SERIAL_TERMIOS
#  include <termios.h>
#endif

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/init.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/semaphore.h>
#include <nuttx/serial/serial.h>
#include <nuttx/kmalloc.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "rk3576_serial.h"
#include "arm64_arch_timer.h"
#include "rk3576_boot.h"
#include "arm64_gic.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_serial.h"

#ifdef USE_SERIALDRIVER

/***************************************************************************
 * Pre-processor Definitions
 ***************************************************************************/


/* UART0 is console and ttyS0, follows U-Boot Bootloader */

#define CONSOLE_DEV     g_uart0port         /* UART0 is console */
#define TTYS0_DEV       g_uart0port         /* UART0 is ttyS0 */

/* UART SCLK is the UART Input Clock.  Through experimentation, it has
 * been found that the serial clock is OSC24M
 */

#define UART_SCLK 24000000

/* Timeout for UART Busy Wait, in milliseconds */

#define UART_TIMEOUT_MS 100

/* UART pin mux and clock gating are configured by the bootloader (U-Boot).
 * RK3576 UART1-4 will get pinctrl/CRU setup here once those drivers land;
 * UART0 (console) needs neither -- the loader leaves it fully configured.
 *
 * UART register definitions are in hardware/rk3576_serial.h.
 *
 * NOTE: Clock gating / software reset live in the RK3576 CRU, not here.
 * The bootloader enables the UART0 clock; a dedicated CRU driver will handle
 * UART1-4 gating/reset when they are brought up.
 */

/***************************************************************************
 * Private Types
 ***************************************************************************/

/* UART Configuration */

struct rk3576_uart_config
{
  unsigned long uart;  /* UART Base Address */
};

/* UART Device Data */

struct rk3576_uart_data
{
  uint32_t baud_rate;  /* UART Baud Rate */
  uint32_t ier;        /* Saved IER value */
  uint8_t  parity;     /* 0=none, 1=odd, 2=even */
  uint8_t  bits;       /* Number of bits (7 or 8) */
  bool     stopbits2;  /* true: Configure with 2 stop bits instead of 1 */
};

/* UART Port */

struct rk3576_uart_port_s
{
  struct rk3576_uart_data data;     /* UART Device Data */
  struct rk3576_uart_config config; /* UART Configuration */
  unsigned int irq_num;          /* UART IRQ Number */
  bool is_console;               /* 1 if this UART is console */
};

/***************************************************************************
 * Private Function Prototypes
 ***************************************************************************/

static void rk3576_uart_rxint(struct uart_dev_s *dev, bool enable);
static void rk3576_uart_txint(struct uart_dev_s *dev, bool enable);

/***************************************************************************
 * Private Functions
 ***************************************************************************/

/***************************************************************************
 * Name: rk3576_uart_divisor
 *
 * Description:
 *   Select a divisor to produce the BAUD from the UART SCLK.
 *
 *     BAUD = SCLK / (16 * DL), or
 *     DL   = SCLK / BAUD / 16
 *
 * Returned Value:
 *   UART Divisor
 *
 ***************************************************************************/

static uint32_t rk3576_uart_divisor(uint32_t baud)
{
  DEBUGASSERT(baud != 0);
  return UART_SCLK / (baud << 4);
}

/***************************************************************************
 * Name: rk3576_uart_irq_handler
 *
 * Description:
 *   This is the common UART interrupt handler.  It should call
 *   uart_xmitchars or uart_recvchars to perform the appropriate data
 *   transfers.
 *
 * Input Parameters:
 *   irq     - IRQ Number
 *   context - Interrupt Context
 *   arg     - UART Device
 *
 * Returned Value:
 *   OK is always returned at present.
 *
 ***************************************************************************/

static int rk3576_uart_irq_handler(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;
  uint32_t status;
  int passes;

  DEBUGASSERT(dev != NULL && dev->priv != NULL);

  /* Loop until there are no characters to be transferred or,
   * until we have been looping for a long time.
   */

  for (passes = 0; passes < 256; passes++)
    {
      /* Get the current UART status */

      status = getreg32(RK3576_UART_IIR(config->uart));

      /* Handle the interrupt by its interrupt ID field */

      switch (status & RK3576_UART_IIR_IID_MASK)
        {
          /* Handle incoming, receive bytes (with or without timeout) */

          case RK3576_UART_IIR_IID_RECV:
          case RK3576_UART_IIR_IID_TIMEOUT:
            {
              uart_recvchars(dev);
              break;
            }

          /* Handle outgoing, transmit bytes */

          case RK3576_UART_IIR_IID_TXEMPTY:
            {
              uart_xmitchars(dev);
              break;
            }

          /* Just clear modem status interrupts (UART1 only) */

          case RK3576_UART_IIR_IID_MODEM:
            {
              /* Read the modem status register (MSR) to clear */

              status = getreg32(RK3576_UART_MSR(config->uart));
              break;
            }

          /* Just clear any line status interrupts */

          case RK3576_UART_IIR_IID_LINESTATUS:
            {
              /* Read the line status register (LSR) to clear */

              status = getreg32(RK3576_UART_LSR(config->uart));
              break;
            }

          /* Busy detect.
           * Just ignore.
           * Cleared by reading the status register
           */

          case RK3576_UART_IIR_IID_BUSY:
            {
              /* Read from the UART status register
               * to clear the BUSY condition
               */

              status = getreg32(RK3576_UART_USR(config->uart));
              break;
            }

          /* No further interrupts pending... return now */

          case RK3576_UART_IIR_IID_NONE:
            {
              return OK;
            }

            /* Otherwise we have received an interrupt
             * that we cannot handle
             */

          default:
            {
              _err("ERROR: Unexpected IIR: %02" PRIx32 "\n", status);
              break;
            }
        }
    }

  return OK;
}

/***************************************************************************
 * Name: rk3576_uart_wait
 *
 * Description:
 *   Wait for UART to be non-busy.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; ERROR if timeout.
 *
 ***************************************************************************/

static int rk3576_uart_wait(struct uart_dev_s *dev)
{
  struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;
  int i;

  for (i = 0; i < UART_TIMEOUT_MS; i++)
    {
      uint32_t status = getreg32(RK3576_UART_USR(config->uart));

      if ((status & RK3576_UART_USR_BUSY) == 0)
        {
          return OK;
        }

      up_mdelay(1);
    }

  _err("UART timeout\n");
  return ERROR;
}

/***************************************************************************
 * Name: rk3576_uart_setup
 *
 * Description:
 *   Configure the UART baud, bits, parity, fifos, etc. This method is
 *   called the first time that the serial port is opened.
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ***************************************************************************/

static int rk3576_uart_setup(struct uart_dev_s *dev)
{
#ifndef CONFIG_SUPPRESS_UART_CONFIG
  struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;
  struct rk3576_uart_data *data = &port->data;
  uint16_t dl;
  uint32_t lcr;
  int ret;

  DEBUGASSERT(data != NULL);

  /* Clear fifos */

  putreg32(RK3576_UART_FCR_RFIFOR | RK3576_UART_FCR_XFIFOR, RK3576_UART_FCR(config->uart));

  /* Set trigger */

  putreg32(RK3576_UART_FCR_FIFOE | RK3576_UART_FCR_RT_HALF, RK3576_UART_FCR(config->uart));

  /* Set up the IER */

  data->ier = getreg32(RK3576_UART_IER(config->uart));

  /* Set up the LCR */

  lcr = 0;

  switch (data->bits)
    {
    case 5:
      lcr |= RK3576_UART_LCR_DLS_5BITS;
      break;

    case 6:
      lcr |= RK3576_UART_LCR_DLS_6BITS;
      break;

    case 7:
      lcr |= RK3576_UART_LCR_DLS_7BITS;
      break;

    case 8:
    default:
      lcr |= RK3576_UART_LCR_DLS_8BITS;
      break;
    }

  if (data->stopbits2)
    {
      lcr |= RK3576_UART_LCR_STOP;
    }

  if (data->parity == 1)
    {
      lcr |= RK3576_UART_LCR_PEN;
    }
  else if (data->parity == 2)
    {
      lcr |= (RK3576_UART_LCR_PEN | RK3576_UART_LCR_EPS);
    }

  /* Set DLAB when UART is not busy */

  ret = rk3576_uart_wait(dev);

  if (ret < 0)
    {
      _err("UART wait failed, ret=%d\n", ret);
      return ret;
    }

  putreg32(lcr | RK3576_UART_LCR_DLAB, RK3576_UART_LCR(config->uart));

  ret = rk3576_uart_wait(dev);

  if (ret < 0)
    {
      _err("UART wait failed, ret=%d\n", ret);
      return ret;
    }

  /* Set the BAUD divisor */

  dl = rk3576_uart_divisor(data->baud_rate);
  putreg8(dl >> 8,   RK3576_UART_DLH(config->uart));
  putreg8(dl & 0xff, RK3576_UART_DLL(config->uart));

  /* Check the BAUD divisor */

  if (getreg8(RK3576_UART_DLH(config->uart)) != (dl >> 8) ||
      getreg8(RK3576_UART_DLL(config->uart)) != (dl & 0xff))
    {
      _err("UART BAUD divisor failed\n");
      return ERROR;
    }

  /* Clear DLAB */

  putreg32(lcr, RK3576_UART_LCR(config->uart));

  /* Configure the FIFOs */

  putreg32(RK3576_UART_FCR_RT_HALF | RK3576_UART_FCR_XFIFOR | RK3576_UART_FCR_RFIFOR |
           RK3576_UART_FCR_FIFOE, RK3576_UART_FCR(config->uart));

  /* Enable Auto-Flow Control in the Modem Control Register */

#if defined(CONFIG_SERIAL_IFLOWCONTROL) || defined(CONFIG_SERIAL_OFLOWCONTROL)
#  warning Missing logic
#endif

#endif /* CONFIG_SUPPRESS_UART_CONFIG */
  return OK;
}

/***************************************************************************
 * Name: rk3576_uart_shutdown
 *
 * Description:
 *   Disable the UART Port.  This method is called when the serial
 *   port is closed.
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_shutdown(struct uart_dev_s *dev)
{
  /* Disable the Receive and Transmit Interrupts */

  rk3576_uart_rxint(dev, false);
  rk3576_uart_txint(dev, false);
}

/***************************************************************************
 * Name: rk3576_uart_attach
 *
 * Description:
 *   Configure the UART to operation in interrupt driven mode.
 *   This method is called when the serial port is opened.
 *   Normally, this is just after the setup() method is called,
 *   however, the serial console may operate in
 *   a non-interrupt driven mode during the boot phase.
 *
 *   RX and TX interrupts are not enabled when by the attach method
 *   (unless the hardware supports multiple levels of interrupt
 *   enabling).  The RX and TX interrupts are not enabled until
 *   the txint() and rxint() methods are called.
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ***************************************************************************/

static int rk3576_uart_attach(struct uart_dev_s *dev)
{
  int ret;
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;

  DEBUGASSERT(port != NULL);

  /* Attach UART Interrupt Handler */

  ret = irq_attach(port->irq_num, rk3576_uart_irq_handler, dev);

  /* Set Interrupt Priority in Generic Interrupt Controller v2 */

  up_prioritize_irq(port->irq_num, 0);
  up_set_irq_type(port->irq_num, IRQ_RISING_EDGE);

  /* Enable UART Interrupt */

  if (ret == OK)
    {
      up_enable_irq(port->irq_num);
    }
  else
    {
      _err("IRQ attach failed, ret=%d\n", ret);
    }

  return ret;
}

/***************************************************************************
 * Name: rk3576_uart_detach
 *
 * Description:
 *   Detach UART interrupts.  This method is called when the serial port is
 *   closed normally just before the shutdown method is called.  The
 *   exception is the serial console which is never shutdown.
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_detach(struct uart_dev_s *dev)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;

  DEBUGASSERT(port != NULL);

  /* Disable UART Interrupt */

  up_disable_irq(port->irq_num);

  /* Detach UART Interrupt Handler */

  irq_detach(port->irq_num);
}

/***************************************************************************
 * Name: rk3576_uart_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method.
 *
 * Input Parameters:
 *   filep - File Struct
 *   cmd   - ioctl Command
 *   arg   - ioctl Argument
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ***************************************************************************/

static int rk3576_uart_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  int ret = OK;

  UNUSED(filep);
  UNUSED(arg);

  switch (cmd)
    {
      case TIOCSBRK:  /* BSD compatibility: Turn break on, unconditionally */
      case TIOCCBRK:  /* BSD compatibility: Turn break off, unconditionally */
      default:
        {
          ret = -ENOTTY;
          break;
        }
    }

  return ret;
}

/***************************************************************************
 * Name: rk3576_uart_receive
 *
 * Description:
 *   Called (usually) from the interrupt level to receive one
 *   character from the UART.  Error bits associated with the
 *   receipt are provided in the return 'status'.
 *
 * Input Parameters:
 *   dev    - UART Device
 *   status - Return status, zero on success
 *
 * Returned Value:
 *   Received character
 *
 ***************************************************************************/

static int rk3576_uart_receive(struct uart_dev_s *dev, unsigned int *status)
{
  struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;
  uint32_t rbr;

  *status = getreg8(RK3576_UART_LSR(config->uart));
  rbr     = getreg8(RK3576_UART_RBR(config->uart));
  return rbr;
}

/***************************************************************************
 * Name: rk3576_uart_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts
 *
 * Input Parameters:
 *   dev    - UART Device
 *   enable - True to enable RX interrupts; false to disable
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_rxint(struct uart_dev_s *dev, bool enable)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;

  /* Write to Interrupt Enable Register (UART_IER) */

  if (enable)
    {
      /* Set ERBFI bit (Enable Rx Data Available Interrupt) */

      modreg8(RK3576_UART_IER_ERBFI, RK3576_UART_IER_ERBFI, RK3576_UART_IER(config->uart));
    }
  else
    {
      /* Clear ERBFI bit (Disable Rx Data Available Interrupt) */

      modreg8(0, RK3576_UART_IER_ERBFI, RK3576_UART_IER(config->uart));
    }
}

/***************************************************************************
 * Name: rk3576_uart_rxavailable
 *
 * Description:
 *   Return true if the Receive FIFO is not empty
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   True if the Receive FIFO is not empty; false otherwise
 *
 ***************************************************************************/

static bool rk3576_uart_rxavailable(struct uart_dev_s *dev)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;

  /* Data Ready Bit (Line Status Register) is 1 if Rx Data is ready */

  return getreg8(RK3576_UART_LSR(config->uart)) & RK3576_UART_LSR_DR;
}

/***************************************************************************
 * Name: rk3576_uart_send
 *
 * Description:
 *   This method will send one byte on the UART
 *
 * Input Parameters:
 *   dev - UART Device
 *   ch  - Character to be sent
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_send(struct uart_dev_s *dev, int ch)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;

  /* Write char to Transmit Holding Register (UART_THR) */

  putreg8(ch, RK3576_UART_THR(config->uart));
}

/***************************************************************************
 * Name: rk3576_uart_txint
 *
 * Description:
 *   Call to enable or disable TX interrupts
 *
 * Input Parameters:
 *   dev    - UART Device
 *   enable - True to enable TX interrupts; false to disable
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_txint(struct uart_dev_s *dev, bool enable)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;

  /* Write to Interrupt Enable Register (UART_IER) */

  if (enable)
    {
      /* Set ETBEI bit (Enable Tx Holding Register Empty Interrupt) */

      modreg8(RK3576_UART_IER_ETBEI, RK3576_UART_IER_ETBEI, RK3576_UART_IER(config->uart));
    }
  else
    {
      /* Clear ETBEI bit (Disable Tx Holding Register Empty Interrupt) */

      modreg8(0, RK3576_UART_IER_ETBEI, RK3576_UART_IER(config->uart));
    }
}

/***************************************************************************
 * Name: rk3576_uart_txready
 *
 * Description:
 *   Return true if the Transmit FIFO is not full
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   True if the Transmit FIFO is not full; false otherwise
 *
 ***************************************************************************/

static bool rk3576_uart_txready(struct uart_dev_s *dev)
{
  const struct rk3576_uart_port_s *port = (struct rk3576_uart_port_s *)dev->priv;
  const struct rk3576_uart_config *config = &port->config;

  /* Tx FIFO is ready if THRE Bit is 1 (Tx Holding Register Empty) */

  return (getreg8(RK3576_UART_LSR(config->uart)) & RK3576_UART_LSR_THRE) != 0;
}

/***************************************************************************
 * Name: rk3576_uart_txempty
 *
 * Description:
 *   Return true if the Transmit FIFO is empty
 *
 * Input Parameters:
 *   dev - UART Device
 *
 * Returned Value:
 *   True if the Transmit FIFO is empty; false otherwise
 *
 ***************************************************************************/

static bool rk3576_uart_txempty(struct uart_dev_s *dev)
{
  /* Tx FIFO is empty if Tx FIFO is not full (for now) */

  return rk3576_uart_txready(dev);
}

/***************************************************************************
 * Name: rk3576_uart_wait_send
 *
 * Description:
 *   Wait for Transmit FIFO until it is not full, then transmit the
 *   character over UART.
 *
 * Input Parameters:
 *   dev - UART Device
 *   ch  - Character to be sent
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

static void rk3576_uart_wait_send(struct uart_dev_s *dev, int ch)
{
  DEBUGASSERT(dev != NULL);
  while (!rk3576_uart_txready(dev));
  rk3576_uart_send(dev, ch);
}

/***************************************************************************
 * Private Data
 ***************************************************************************/

/* UART Operations for Serial Driver */

static const struct uart_ops_s g_uart_ops =
{
  .setup    = rk3576_uart_setup,
  .shutdown = rk3576_uart_shutdown,
  .attach   = rk3576_uart_attach,
  .detach   = rk3576_uart_detach,
  .ioctl    = rk3576_uart_ioctl,
  .receive  = rk3576_uart_receive,
  .rxint    = rk3576_uart_rxint,
  .rxavailable = rk3576_uart_rxavailable,
#ifdef CONFIG_SERIAL_IFLOWCONTROL
  .rxflowcontrol    = NULL,
#endif
  .send     = rk3576_uart_send,
  .txint    = rk3576_uart_txint,
  .txready  = rk3576_uart_txready,
  .txempty  = rk3576_uart_txempty,
};

/* UART0 Port State (Console) */

#ifndef CONFIG_UART0_BAUD
#  define CONFIG_UART0_BAUD 115200
#endif

#ifndef CONFIG_UART0_BITS
#  define CONFIG_UART0_BITS 8
#endif

#ifndef CONFIG_UART0_PARITY
#  define CONFIG_UART0_PARITY 0
#endif

#ifndef CONFIG_UART0_2STOP
#  define CONFIG_UART0_2STOP 0
#endif

#ifndef CONFIG_UART0_RXBUFSIZE
#  define CONFIG_UART0_RXBUFSIZE 256
#endif

#ifndef CONFIG_UART0_TXBUFSIZE
#  define CONFIG_UART0_TXBUFSIZE 256
#endif

#ifdef CONFIG_RK3576_UART
static struct rk3576_uart_port_s g_uart0priv =
{
  .data   =
    {
      .baud_rate  = CONFIG_UART0_BAUD,
      .parity     = CONFIG_UART0_PARITY,
      .bits       = CONFIG_UART0_BITS,
      .stopbits2  = CONFIG_UART0_2STOP
    },

  .config =
    {
      .uart       = RK3576_UART0_ADDR
    },

    .irq_num      = RK3576_IRQ_UART0,
    .is_console   = 1
};

/* UART0 I/O Buffers (Console) */

static char g_uart0rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_uart0txbuffer[CONFIG_UART0_TXBUFSIZE];

/* UART0 Port Definition (Console) */

static struct uart_dev_s g_uart0port =
{
  .recv  =
    {
      .size   = CONFIG_UART0_RXBUFSIZE,
      .buffer = g_uart0rxbuffer,
    },

  .xmit  =
    {
      .size   = CONFIG_UART0_TXBUFSIZE,
      .buffer = g_uart0txbuffer,
    },

  .ops   = &g_uart_ops,
  .priv  = &g_uart0priv,
};

#endif /* CONFIG_RK3576_UART */

/* Look-up table for dynamic UART registration (UART1 ~ UART11).
 * UART0 is excluded because it is the console and is registered statically.
 * All entries are unconditional — board code decides which UARTs to
 * actually register at runtime via rk3576_serial_register().
 */

struct rk3576_uart_hwdesc_s
{
  uintptr_t base;     /* Register base address */
  unsigned int irq;   /* IRQ number */
};

static const struct rk3576_uart_hwdesc_s g_uart_hwdesc[UART_PORT_MAX + 1] =
{
  [UART_PORT_1]  = { .base = RK3576_UART1_ADDR,  .irq = RK3576_IRQ_UART1  },
  [UART_PORT_2]  = { .base = RK3576_UART2_ADDR,  .irq = RK3576_IRQ_UART2  },
  [UART_PORT_3]  = { .base = RK3576_UART3_ADDR,  .irq = RK3576_IRQ_UART3  },
  [UART_PORT_4]  = { .base = RK3576_UART4_ADDR,  .irq = RK3576_IRQ_UART4  },
  [UART_PORT_5]  = { .base = RK3576_UART5_ADDR,  .irq = RK3576_IRQ_UART5  },
  [UART_PORT_6]  = { .base = RK3576_UART6_ADDR,  .irq = RK3576_IRQ_UART6  },
  [UART_PORT_7]  = { .base = RK3576_UART7_ADDR,  .irq = RK3576_IRQ_UART7  },
  [UART_PORT_8]  = { .base = RK3576_UART8_ADDR,  .irq = RK3576_IRQ_UART8  },
  [UART_PORT_9]  = { .base = RK3576_UART9_ADDR,  .irq = RK3576_IRQ_UART9  },
  [UART_PORT_10] = { .base = RK3576_UART10_ADDR, .irq = RK3576_IRQ_UART10 },
  [UART_PORT_11] = { .base = RK3576_UART11_ADDR, .irq = RK3576_IRQ_UART11 },
};

/***************************************************************************
 * Public Functions
 ***************************************************************************/

/***************************************************************************
 * Name: arm64_earlyserialinit
 *
 * Description:
 *   Performs the low level UART initialization early in
 *   debug so that the serial console will be available
 *   during bootup.  This must be called before arm64_serialinit.
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

void arm64_earlyserialinit(void)
{
  /* NOTE: This function assumes that UART0 low level hardware configuration
   * -- including all clocking and pin configuration -- was performed
   * earlier by U-Boot Bootloader.
   */

  /* UART1-11 need CRU clock + pinctrl setup here once those drivers exist.
   * Until then only the console (UART0, configured by the bootloader) is
   * brought up below.
   */

#ifdef CONSOLE_DEV
  /* Enable the console at UART0 */

  CONSOLE_DEV.isconsole = true;
  rk3576_uart_setup(&CONSOLE_DEV);
#endif
}

/***************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug
 *   writes
 *
 * Input Parameters:
 *   ch - Character to be transmitted over UART
 *
 * Returned Value:
 *   Character that was transmitted
 *
 ***************************************************************************/

void up_putc(int ch)
{
#ifdef CONSOLE_DEV
  struct uart_dev_s *dev = &CONSOLE_DEV;

  rk3576_uart_wait_send(dev, ch);
#endif
}

/***************************************************************************
 * Name: arm64_serialinit
 *
 * Description:
 *   Register serial console and serial ports.  This assumes
 *   that arm64_earlyserialinit was called previously.
 *
 * Returned Value:
 *   None
 *
 ***************************************************************************/

void arm64_serialinit(void)
{
#ifdef CONSOLE_DEV
  int ret;

  /* Register the console (UART0).  Non-console UARTs are registered
   * dynamically by board code via rk3576_serial_register().
   */

  ret = uart_register("/dev/console", &CONSOLE_DEV);
  if (ret < 0)
    {
      _err("Register /dev/console failed, ret=%d\n", ret);
    }

  ret = uart_register("/dev/ttyS0", &TTYS0_DEV);
  if (ret < 0)
    {
      _err("Register /dev/ttyS0 failed, ret=%d\n", ret);
    }
#endif
}

/***************************************************************************
 * Name: rk3576_serial_register
 *
 * Description:
 *   Dynamically allocate and register a non-console UART port (UART1~11).
 *   Called by board-level code (e.g. board_late_initialize) after the heap
 *   is available.  UART0 must NOT be registered through this function.
 *
 * Input Parameters:
 *   port_id - UART port identifier (UART_PORT_1 ~ UART_PORT_11)
 *   baud    - Baud rate
 *   bits    - Data bits (5 ~ 8)
 *   parity  - 0=none, 1=odd, 2=even
 *   stop2   - true = 2 stop bits, false = 1 stop bit
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ***************************************************************************/

int rk3576_serial_register(uint8_t port_id, uint32_t baud,
                           uint8_t bits, uint8_t parity, bool stop2)
{
  struct rk3576_uart_port_s *priv;
  struct uart_dev_s *dev;
  char *rxbuf;
  char *txbuf;
  char devname[16];
  const struct rk3576_uart_hwdesc_s *hw;
  int ret;

  /* Validate port_id */

  if (port_id < UART_PORT_1 || port_id > UART_PORT_MAX)
    {
      _err("Invalid port_id=%u\n", port_id);
      return -EINVAL;
    }

  hw = &g_uart_hwdesc[port_id];

  /* Allocate port private data */

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      _err("UART%u: kmm_zalloc priv failed\n", port_id);
      return -ENOMEM;
    }

  /* Allocate uart_dev_s */

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      _err("UART%u: kmm_zalloc dev failed\n", port_id);
      kmm_free(priv);
      return -ENOMEM;
    }

  /* Allocate RX and TX buffers */

#define UART_DYN_RXBUFSIZE 256
#define UART_DYN_TXBUFSIZE 256

  rxbuf = kmm_zalloc(UART_DYN_RXBUFSIZE);
  txbuf = kmm_zalloc(UART_DYN_TXBUFSIZE);
  if (rxbuf == NULL || txbuf == NULL)
    {
      _err("UART%u: kmm_zalloc buffers failed\n", port_id);
      if (rxbuf) kmm_free(rxbuf);
      if (txbuf) kmm_free(txbuf);
      kmm_free(dev);
      kmm_free(priv);
      return -ENOMEM;
    }

  /* Fill port private data */

  priv->data.baud_rate = baud;
  priv->data.parity    = parity;
  priv->data.bits      = bits;
  priv->data.stopbits2 = stop2;
  priv->config.uart    = hw->base;
  priv->irq_num        = hw->irq;
  priv->is_console     = false;

  /* Fill uart_dev_s */

  dev->recv.size   = UART_DYN_RXBUFSIZE;
  dev->recv.buffer = rxbuf;
  dev->xmit.size   = UART_DYN_TXBUFSIZE;
  dev->xmit.buffer = txbuf;
  dev->ops         = &g_uart_ops;
  dev->priv        = priv;

  /* Register the device as /dev/ttySx (x = port_id) */

  snprintf(devname, sizeof(devname), "/dev/ttyS%u", port_id);
  ret = uart_register(devname, dev);
  if (ret < 0)
    {
      _err("UART%u: uart_register %s failed, ret=%d\n",
           port_id, devname, ret);
      kmm_free(txbuf);
      kmm_free(rxbuf);
      kmm_free(dev);
      kmm_free(priv);
      return ret;
    }

  return OK;
}

#endif /* USE_SERIALDRIVER */
