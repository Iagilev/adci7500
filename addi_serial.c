// SPDX-License-Identifier: GPL-2.0
/*
 *  Probe module for 8250/16550-type PCI ADDI-DATA serial ports.
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 *  Copyright (C) 2001 Russell King, All Rights Reserved.
 */
#undef DEBUG
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/tty.h>
#include <linux/serial_reg.h>
#include <linux/serial_core.h>
#include <linux/8250_pci.h>
#include <linux/bitops.h>

#include <asm/byteorder.h>
#include <asm/io.h>

#include "8250.h"

/*
 * init function returns:
 *  > 0 - number of ports
 *  = 0 - use board->num_ports
 *  < 0 - error
 */
struct pci_serial_quirk
{
	u32 vendor;
	u32 device;
	u32 subvendor;
	u32 subdevice;
	int (*probe)(struct pci_dev *dev);
	int (*init)(struct pci_dev *dev);
	int (*setup)(struct serial_private *,
				 const struct pciserial_board *,
				 struct uart_8250_port *, int);
	void (*exit)(struct pci_dev *dev);
};

#define PCI_NUM_BAR_RESOURCES 6

struct serial_private
{
	struct pci_dev *dev;
	unsigned int nr;
	struct pci_serial_quirk *quirk;
	const struct pciserial_board *board;
	int line[0];
};

static int pci_default_setup(struct serial_private *,
							 const struct pciserial_board *, struct uart_8250_port *, int);

static void moan_device(const char *str, struct pci_dev *dev)
{
	dev_err(&dev->dev,
			"%s: %s\n"
			"Please send the output of lspci -vv, this\n"
			"message (0x%04x,0x%04x,0x%04x,0x%04x), the\n"
			"manufacturer and name of serial board or\n"
			"modem board to <linux-serial@vger.kernel.org>.\n",
			pci_name(dev), str, dev->vendor, dev->device,
			dev->subsystem_vendor, dev->subsystem_device);
}

static int
setup_port(struct serial_private *priv, struct uart_8250_port *port,
		   int bar, int offset, int regshift)
{
	struct pci_dev *dev = priv->dev;

	if (bar >= PCI_NUM_BAR_RESOURCES)
		return -EINVAL;

	if (pci_resource_flags(dev, bar) & IORESOURCE_MEM)
	{
		if (!pcim_iomap(dev, bar, 0) && !pcim_iomap_table(dev))
			return -ENOMEM;

		port->port.iotype = UPIO_MEM;
		port->port.iobase = 0;
		port->port.mapbase = pci_resource_start(dev, bar) + offset;
		port->port.membase = pcim_iomap_table(dev)[bar] + offset;
		port->port.regshift = regshift;
	}
	else
	{
		port->port.iotype = UPIO_PORT;
		port->port.iobase = pci_resource_start(dev, bar) + offset;
		port->port.mapbase = 0;
		port->port.membase = NULL;
		port->port.regshift = 0;
	}
	return 0;
}

/*
 * ADDI-DATA GmbH communication cards <info@addi-data.com>
 */
static int addidata_apci7800_setup(struct serial_private *priv,
								   const struct pciserial_board *board,
								   struct uart_8250_port *port, int idx)
{
	unsigned int bar = 0, offset = board->first_offset;
	bar = FL_GET_BASE(board->flags);

	if (idx < 2)
	{
		offset += idx * board->uart_offset;
	}
	else if ((idx >= 2) && (idx < 4))
	{
		bar += 1;
		offset += ((idx - 2) * board->uart_offset);
	}
	else if ((idx >= 4) && (idx < 6))
	{
		bar += 2;
		offset += ((idx - 4) * board->uart_offset);
	}
	else if (idx >= 6)
	{
		bar += 3;
		offset += ((idx - 6) * board->uart_offset);
	}

	return setup_port(priv, port, bar, offset, board->reg_shift);
}

static int pci_default_setup(struct serial_private *priv,
							 const struct pciserial_board *board,
							 struct uart_8250_port *port, int idx)
{
	unsigned int bar, offset = board->first_offset, maxnr;

	bar = FL_GET_BASE(board->flags);
	if (board->flags & FL_BASE_BARS)
		bar += idx;
	else
		offset += idx * board->uart_offset;

	maxnr = (pci_resource_len(priv->dev, bar) - board->first_offset) >>
			(board->reg_shift + 3);

	if (board->flags & FL_REGION_SZ_CAP && idx >= maxnr)
		return 1;

	return setup_port(priv, port, bar, offset, board->reg_shift);
}

#define PCI_DEVICE_ID_AMCC_ADDIDATA_APCI7800 0x818e
#define PCI_DEVICE_ID_AMCC_ADDIDATA_APCI7500_d3 0x7003

/* Unknown vendors/cards - this should not be in linux/pci_ids.h */
#define PCI_SUBDEVICE_ID_UNKNOWN_0x1584 0x1584
#define PCI_SUBDEVICE_ID_UNKNOWN_0x1588 0x1588

/*
 * Master list of serial port init/setup/exit quirks.
 * This does not describe the general nature of the port.
 * (ie, baud base, number and location of ports, etc)
 *
 * This list is ordered alphabetically by vendor then device.
 * Specific entries must come before more generic entries.
 */
static struct pci_serial_quirk pci_serial_quirks[] __refdata = {
	/*
	* ADDI-DATA GmbH communication cards <info@addi-data.com>
	*/
	{
		.vendor = PCI_VENDOR_ID_AMCC,
		.device = PCI_DEVICE_ID_AMCC_ADDIDATA_APCI7800,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.setup = addidata_apci7800_setup,
	},

	/*
	 * Default "match everything" terminator entry
	 */
	{
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.setup = pci_default_setup,
	}};

static inline int quirk_id_matches(u32 quirk_id, u32 dev_id)
{
	return quirk_id == PCI_ANY_ID || quirk_id == dev_id;
}

static struct pci_serial_quirk *find_quirk(struct pci_dev *dev)
{
	struct pci_serial_quirk *quirk;

	for (quirk = pci_serial_quirks;; quirk++)
		if (quirk_id_matches(quirk->vendor, dev->vendor) &&
			quirk_id_matches(quirk->device, dev->device) &&
			quirk_id_matches(quirk->subvendor, dev->subsystem_vendor) &&
			quirk_id_matches(quirk->subdevice, dev->subsystem_device))
			break;
	return quirk;
}

static inline int get_pci_irq(struct pci_dev *dev,
							  const struct pciserial_board *board)
{
	if (board->flags & FL_NOIRQ)
		return 0;
	else
		return dev->irq;
}

/*
 * This is the configuration table for all of the PCI serial boards
 * which we support.  It is directly indexed by the pci_board_num_t enum
 * value, which is encoded in the pci_device_id PCI probe table's
 * driver_data member.
 *
 * The makeup of these names are:
 *  pbn_bn{_bt}_n_baud{_offsetinhex}
 *
 *  bn		= PCI BAR number
 *  bt		= Index using PCI BARs
 *  n		= number of serial ports
 *  baud	= baud rate
 *  offsetinhex	= offset for each sequential port (in hex)
 *
 * This table is sorted by (in order): bn, bt, baud, offsetindex, n.
 *
 * Please note: in theory if n = 1, _bt infix should make no difference.
 * ie, pbn_b0_1_115200 is the same as pbn_b0_bt_1_115200
 */
enum pci_board_num_t
{
	pbn_default = 0,

	pbn_b0_1_115200,
	pbn_b0_2_115200,
	pbn_b0_4_115200,
	pbn_b0_5_115200,
	pbn_b0_8_115200,

	pbn_b0_1_921600,
	pbn_b0_2_921600,
	pbn_b0_4_921600,

	pbn_b0_2_1130000,

	pbn_b0_4_1152000,

	pbn_b0_4_1250000,

	pbn_b0_2_1843200,
	pbn_b0_4_1843200,

	pbn_b0_1_4000000,

	pbn_b0_bt_1_115200,
	pbn_b0_bt_2_115200,
	pbn_b0_bt_4_115200,
	pbn_b0_bt_8_115200,

	pbn_b0_bt_1_460800,
	pbn_b0_bt_2_460800,
	pbn_b0_bt_4_460800,

	pbn_b0_bt_1_921600,
	pbn_b0_bt_2_921600,
	pbn_b0_bt_4_921600,
	pbn_b0_bt_8_921600,

	pbn_b1_8_115200,
	/*
	 * Board-specific versions.
	 */
	pbn_ADDIDATA_PCIe_1_3906250,
	pbn_ADDIDATA_PCIe_2_3906250,
	pbn_ADDIDATA_PCIe_4_3906250,
	pbn_ADDIDATA_PCIe_8_3906250
};

/*
 * uart_offset - the space between channels
 * reg_shift   - describes how the UART registers are mapped
 *               to PCI memory by the card.
 * For example IER register on SBS, Inc. PMC-OctPro is located at
 * offset 0x10 from the UART base, while UART_IER is defined as 1
 * in include/linux/serial_reg.h,
 * see first lines of serial_in() and serial_out() in 8250.c
*/

static struct pciserial_board pci_boards[] = {
	[pbn_default] = {
		.flags = FL_BASE0,
		.num_ports = 1,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_1_115200] = {
		.flags = FL_BASE0,
		.num_ports = 1,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_2_115200] = {
		.flags = FL_BASE0,
		.num_ports = 2,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_4_115200] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_5_115200] = {
		.flags = FL_BASE0,
		.num_ports = 5,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_8_115200] = {
		.flags = FL_BASE0,
		.num_ports = 8,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_1_921600] = {
		.flags = FL_BASE0,
		.num_ports = 1,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	[pbn_b0_2_921600] = {
		.flags = FL_BASE0,
		.num_ports = 2,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	[pbn_b0_4_921600] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 921600,
		.uart_offset = 8,
	},

	[pbn_b0_2_1130000] = {
		.flags = FL_BASE0,
		.num_ports = 2,
		.base_baud = 1130000,
		.uart_offset = 8,
	},

	[pbn_b0_4_1152000] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 1152000,
		.uart_offset = 8,
	},

	[pbn_b0_4_1250000] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 1250000,
		.uart_offset = 8,
	},

	[pbn_b0_2_1843200] = {
		.flags = FL_BASE0,
		.num_ports = 2,
		.base_baud = 1843200,
		.uart_offset = 8,
	},
	[pbn_b0_4_1843200] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 1843200,
		.uart_offset = 8,
	},

	[pbn_b0_1_4000000] = {
		.flags = FL_BASE0,
		.num_ports = 1,
		.base_baud = 4000000,
		.uart_offset = 8,
	},

	[pbn_b0_bt_1_115200] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 1,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_bt_2_115200] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 2,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_bt_4_115200] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 4,
		.base_baud = 115200,
		.uart_offset = 8,
	},
	[pbn_b0_bt_8_115200] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 8,
		.base_baud = 115200,
		.uart_offset = 8,
	},

	[pbn_b0_bt_1_460800] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 1,
		.base_baud = 460800,
		.uart_offset = 8,
	},
	[pbn_b0_bt_2_460800] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 2,
		.base_baud = 460800,
		.uart_offset = 8,
	},
	[pbn_b0_bt_4_460800] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 4,
		.base_baud = 460800,
		.uart_offset = 8,
	},

	[pbn_b0_bt_1_921600] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 1,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	[pbn_b0_bt_2_921600] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 2,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	[pbn_b0_bt_4_921600] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 4,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	[pbn_b0_bt_8_921600] = {
		.flags = FL_BASE0 | FL_BASE_BARS,
		.num_ports = 8,
		.base_baud = 921600,
		.uart_offset = 8,
	},
	/*
	 * ADDI-DATA GmbH PCI-Express communication cards <info@addi-data.com>
	 */
	[pbn_ADDIDATA_PCIe_1_3906250] = {
		.flags = FL_BASE0,
		.num_ports = 1,
		.base_baud = 3906250,
		.uart_offset = 0x200,
		.first_offset = 0x1000,
	},
	[pbn_ADDIDATA_PCIe_2_3906250] = {
		.flags = FL_BASE0,
		.num_ports = 2,
		.base_baud = 3906250,
		.uart_offset = 0x200,
		.first_offset = 0x1000,
	},
	[pbn_ADDIDATA_PCIe_4_3906250] = {
		.flags = FL_BASE0,
		.num_ports = 4,
		.base_baud = 3906250,
		.uart_offset = 0x200,
		.first_offset = 0x1000,
	},
	[pbn_ADDIDATA_PCIe_8_3906250] = {
		.flags = FL_BASE0,
		.num_ports = 8,
		.base_baud = 3906250,
		.uart_offset = 0x200,
		.first_offset = 0x1000,
	}};

static int serial_pci_is_class_communication(struct pci_dev *dev)
{
	/*
	 * If it is not a communications device or the programming
	 * interface is greater than 6, give up.
	 */
	if ((((dev->class >> 8) != PCI_CLASS_COMMUNICATION_SERIAL) &&
		 ((dev->class >> 8) != PCI_CLASS_COMMUNICATION_MULTISERIAL) &&
		 ((dev->class >> 8) != PCI_CLASS_COMMUNICATION_MODEM)) ||
		(dev->class & 0xff) > 6)
		return -ENODEV;

	return 0;
}

/*
 * Given a complete unknown PCI device, try to use some heuristics to
 * guess what the configuration might be, based on the pitiful PCI
 * serial specs.  Returns 0 on success, -ENODEV on failure.
 */
static int
serial_pci_guess_board(struct pci_dev *dev, struct pciserial_board *board)
{
	int num_iomem, num_port, first_port = -1, i;
	int rc;

	rc = serial_pci_is_class_communication(dev);
	if (rc)
		return rc;

	/*
	 * Should we try to make guesses for multiport serial devices later?
	 */
	if ((dev->class >> 8) == PCI_CLASS_COMMUNICATION_MULTISERIAL)
		return -ENODEV;

	num_iomem = num_port = 0;
	for (i = 0; i < PCI_NUM_BAR_RESOURCES; i++)
	{
		if (pci_resource_flags(dev, i) & IORESOURCE_IO)
		{
			num_port++;
			if (first_port == -1)
				first_port = i;
		}
		if (pci_resource_flags(dev, i) & IORESOURCE_MEM)
			num_iomem++;
	}

	/*
	 * If there is 1 or 0 iomem regions, and exactly one port,
	 * use it.  We guess the number of ports based on the IO
	 * region size.
	 */
	if (num_iomem <= 1 && num_port == 1)
	{
		board->flags = first_port;
		board->num_ports = pci_resource_len(dev, first_port) / 8;
		return 0;
	}

	/*
	 * Now guess if we've got a board which indexes by BARs.
	 * Each IO BAR should be 8 bytes, and they should follow
	 * consecutively.
	 */
	first_port = -1;
	num_port = 0;
	for (i = 0; i < PCI_NUM_BAR_RESOURCES; i++)
	{
		if (pci_resource_flags(dev, i) & IORESOURCE_IO &&
			pci_resource_len(dev, i) == 8 &&
			(first_port == -1 || (first_port + num_port) == i))
		{
			num_port++;
			if (first_port == -1)
				first_port = i;
		}
	}

	if (num_port > 1)
	{
		board->flags = first_port | FL_BASE_BARS;
		board->num_ports = num_port;
		return 0;
	}

	return -ENODEV;
}

static inline int
serial_pci_matches(const struct pciserial_board *board,
				   const struct pciserial_board *guessed)
{
	return board->num_ports == guessed->num_ports &&
		   board->base_baud == guessed->base_baud &&
		   board->uart_offset == guessed->uart_offset &&
		   board->reg_shift == guessed->reg_shift &&
		   board->first_offset == guessed->first_offset;
}

struct serial_private *
addi_pciserial_init_ports(struct pci_dev *dev, const struct pciserial_board *board)
{
	struct uart_8250_port uart;
	struct serial_private *priv;
	struct pci_serial_quirk *quirk;
	int rc, nr_ports, i;

	nr_ports = board->num_ports;

	/*
	 * Find an init and setup quirks.
	 */
	quirk = find_quirk(dev);

	/*
	 * Run the new-style initialization function.
	 * The initialization function returns:
	 *  <0  - error
	 *   0  - use board->num_ports
	 *  >0  - number of ports
	 */
	if (quirk->init)
	{
		rc = quirk->init(dev);
		if (rc < 0)
		{
			priv = ERR_PTR(rc);
			goto err_out;
		}
		if (rc)
			nr_ports = rc;
	}

	priv = kzalloc(sizeof(struct serial_private) +
					   sizeof(unsigned int) * nr_ports,
				   GFP_KERNEL);
	if (!priv)
	{
		priv = ERR_PTR(-ENOMEM);
		goto err_deinit;
	}

	priv->dev = dev;
	priv->quirk = quirk;

	memset(&uart, 0, sizeof(uart));
	uart.port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	uart.port.uartclk = board->base_baud * 16;
	uart.port.irq = get_pci_irq(dev, board);
	uart.port.dev = &dev->dev;

	for (i = 0; i < nr_ports; i++)
	{
		if (quirk->setup(priv, board, &uart, i))
			break;

		dev_dbg(&dev->dev, "Setup PCI port: port %lx, irq %d, type %d\n",
				uart.port.iobase, uart.port.irq, uart.port.iotype);

		priv->line[i] = serial8250_register_8250_port(&uart);
		if (priv->line[i] < 0)
		{
			dev_err(&dev->dev,
					"Couldn't register serial port %lx, irq %d, type %d, error %d\n",
					uart.port.iobase, uart.port.irq,
					uart.port.iotype, priv->line[i]);
			break;
		}
	}
	priv->nr = i;
	priv->board = board;
	return priv;

err_deinit:
	if (quirk->exit)
		quirk->exit(dev);
err_out:
	return priv;
}
EXPORT_SYMBOL_GPL(addi_pciserial_init_ports);

static void pciserial_detach_ports(struct serial_private *priv)
{
	struct pci_serial_quirk *quirk;
	int i;

	for (i = 0; i < priv->nr; i++)
		serial8250_unregister_port(priv->line[i]);

	/*
	 * Find the exit quirks.
	 */
	quirk = find_quirk(priv->dev);
	if (quirk->exit)
		quirk->exit(priv->dev);
}

void addi_pciserial_remove_ports(struct serial_private *priv)
{
	pciserial_detach_ports(priv);
	kfree(priv);
}
EXPORT_SYMBOL_GPL(addi_pciserial_remove_ports);

void addi_pciserial_suspend_ports(struct serial_private *priv)
{
	int i;

	for (i = 0; i < priv->nr; i++)
		if (priv->line[i] >= 0)
			serial8250_suspend_port(priv->line[i]);

	/*
	 * Ensure that every init quirk is properly torn down
	 */
	if (priv->quirk->exit)
		priv->quirk->exit(priv->dev);
}
EXPORT_SYMBOL_GPL(addi_pciserial_suspend_ports);

void addi_pciserial_resume_ports(struct serial_private *priv)
{
	int i;

	/*
	 * Ensure that the board is correctly configured.
	 */
	if (priv->quirk->init)
		priv->quirk->init(priv->dev);

	for (i = 0; i < priv->nr; i++)
		if (priv->line[i] >= 0)
			serial8250_resume_port(priv->line[i]);
}
EXPORT_SYMBOL_GPL(addi_pciserial_resume_ports);

/*
 * Probe one serial board.  Unfortunately, there is no rhyme nor reason
 * to the arrangement of serial ports on a PCI card.
 */
static int
pciserial_init_one(struct pci_dev *dev, const struct pci_device_id *ent)
{
	struct pci_serial_quirk *quirk;
	struct serial_private *priv;
	const struct pciserial_board *board;
	struct pciserial_board tmp;
	int rc;

	quirk = find_quirk(dev);
	if (quirk->probe)
	{
		rc = quirk->probe(dev);
		if (rc)
			return rc;
	}

	if (ent->driver_data >= ARRAY_SIZE(pci_boards))
	{
		dev_err(&dev->dev, "invalid driver_data: %ld\n",
				ent->driver_data);
		return -EINVAL;
	}

	board = &pci_boards[ent->driver_data];

	rc = pcim_enable_device(dev);
	pci_save_state(dev);
	if (rc)
		return rc;

	if (ent->driver_data == pbn_default)
	{
		/*
		 * Use a copy of the pci_board entry for this;
		 * avoid changing entries in the table.
		 */
		memcpy(&tmp, board, sizeof(struct pciserial_board));
		board = &tmp;

		/*
		 * We matched one of our class entries.  Try to
		 * determine the parameters of this board.
		 */
		rc = serial_pci_guess_board(dev, &tmp);
		if (rc)
			return rc;
	}
	else
	{
		/*
		 * We matched an explicit entry.  If we are able to
		 * detect this boards settings with our heuristic,
		 * then we no longer need this entry.
		 */
		memcpy(&tmp, &pci_boards[pbn_default],
			   sizeof(struct pciserial_board));
		rc = serial_pci_guess_board(dev, &tmp);
		if (rc == 0 && serial_pci_matches(board, &tmp))
			moan_device("Redundant entry in serial pci_table.",
						dev);
	}

	priv = addi_pciserial_init_ports(dev, board);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	pci_set_drvdata(dev, priv);
	return 0;
}

static void pciserial_remove_one(struct pci_dev *dev)
{
	struct serial_private *priv = pci_get_drvdata(dev);

	addi_pciserial_remove_ports(priv);
}

#ifdef CONFIG_PM_SLEEP
static int pciserial_suspend_one(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct serial_private *priv = pci_get_drvdata(pdev);

	if (priv)
		addi_pciserial_suspend_ports(priv);

	return 0;
}

static int pciserial_resume_one(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct serial_private *priv = pci_get_drvdata(pdev);
	int err;

	if (priv)
	{
		/*
		 * The device may have been disabled.  Re-enable it.
		 */
		err = pci_enable_device(pdev);
		/* FIXME: We cannot simply error out here */
		if (err)
			dev_err(dev, "Unable to re-enable ports, trying to continue.\n");
		addi_pciserial_resume_ports(priv);
	}
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(pciserial_pm_ops, pciserial_suspend_one,
						 pciserial_resume_one);

static const struct pci_device_id serial_pci_tbl[] = {
	/*
	* ADDI-DATA GmbH communication cards <info@addi-data.com>
	*/
	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7500,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_4_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_AMCC_ADDIDATA_APCI7500_d3,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_4_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7420,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_2_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7300,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_1_115200},

	{PCI_VENDOR_ID_AMCC,
	 PCI_DEVICE_ID_AMCC_ADDIDATA_APCI7800,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b1_8_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7500_2,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_4_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7420_2,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_2_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7300_2,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_1_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7500_3,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_4_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7420_3,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_2_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7300_3,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_1_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCI7800_3,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_b0_8_115200},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCIe7500,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_ADDIDATA_PCIe_4_3906250},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCIe7420,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_ADDIDATA_PCIe_2_3906250},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCIe7300,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_ADDIDATA_PCIe_1_3906250},

	{PCI_VENDOR_ID_ADDIDATA,
	 PCI_DEVICE_ID_ADDIDATA_APCIe7800,
	 PCI_ANY_ID,
	 PCI_ANY_ID,
	 0,
	 0,
	 pbn_ADDIDATA_PCIe_8_3906250},

	{PCI_VENDOR_ID_NETMOS, PCI_DEVICE_ID_NETMOS_9835,
	 PCI_VENDOR_ID_IBM, 0x0299,
	 0, 0, pbn_b0_bt_2_115200},

	{
		0,
	}
};

static pci_ers_result_t serial8250_io_error_detected(struct pci_dev *dev,
													 pci_channel_state_t state)
{
	struct serial_private *priv = pci_get_drvdata(dev);

	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	if (priv)
		pciserial_detach_ports(priv);

	pci_disable_device(dev);

	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t serial8250_io_slot_reset(struct pci_dev *dev)
{
	int rc;

	rc = pci_enable_device(dev);

	if (rc)
		return PCI_ERS_RESULT_DISCONNECT;

	pci_restore_state(dev);
	pci_save_state(dev);

	return PCI_ERS_RESULT_RECOVERED;
}

static void serial8250_io_resume(struct pci_dev *dev)
{
	struct serial_private *priv = pci_get_drvdata(dev);
	struct serial_private *new;

	if (!priv)
		return;

	new = addi_pciserial_init_ports(dev, priv->board);
	if (!IS_ERR(new))
	{
		pci_set_drvdata(dev, new);
		kfree(priv);
	}
}

static const struct pci_error_handlers serial8250_err_handler = {
	.error_detected = serial8250_io_error_detected,
	.slot_reset = serial8250_io_slot_reset,
	.resume = serial8250_io_resume,
};

static struct pci_driver serial_pci_driver = {
	.name = "addi_serial",
	.probe = pciserial_init_one,
	.remove = pciserial_remove_one,
	.driver = {
		.pm = &pciserial_pm_ops,
	},
	.id_table = serial_pci_tbl,
	.err_handler = &serial8250_err_handler,
};

module_pci_driver(serial_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Generic 8250/16x50 PCI ADDI-DATA serial probe module");
MODULE_DEVICE_TABLE(pci, serial_pci_tbl);
