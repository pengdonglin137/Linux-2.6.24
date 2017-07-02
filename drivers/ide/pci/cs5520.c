/*
 *	IDE tuning and bus mastering support for the CS5510/CS5520
 *	chipsets
 *
 *	The CS5510/CS5520 are slightly unusual devices. Unlike the 
 *	typical IDE controllers they do bus mastering with the drive in
 *	PIO mode and smarter silicon.
 *
 *	The practical upshot of this is that we must always tune the
 *	drive for the right PIO mode. We must also ignore all the blacklists
 *	and the drive bus mastering DMA information.
 *
 *	*** This driver is strictly experimental ***
 *
 *	(c) Copyright Red Hat Inc 2002
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * For the avoidance of doubt the "preferred form" of this code is one which
 * is in an open non patent encumbered format. Where cryptographic key signing
 * forms part of the process of creating an executable the information
 * including keys needed to generate an equivalently functional executable
 * are deemed to be part of the source code.
 *
 */
 
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>

#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/ide.h>
#include <linux/dma-mapping.h>

#include <asm/io.h>
#include <asm/irq.h>

struct pio_clocks
{
	int address;
	int assert;
	int recovery;
};

static struct pio_clocks cs5520_pio_clocks[]={
	{3, 6, 11},
	{2, 5, 6},
	{1, 4, 3},
	{1, 3, 2},
	{1, 2, 1}
};

static void cs5520_set_pio_mode(ide_drive_t *drive, const u8 pio)
{
	ide_hwif_t *hwif = HWIF(drive);
	struct pci_dev *pdev = hwif->pci_dev;
	int controller = drive->dn > 1 ? 1 : 0;
	u8 reg;

	/* FIXME: if DMA = 1 do we need to set the DMA bit here ? */

	/* 8bit CAT/CRT - 8bit command timing for channel */
	pci_write_config_byte(pdev, 0x62 + controller, 
		(cs5520_pio_clocks[pio].recovery << 4) |
		(cs5520_pio_clocks[pio].assert));

	/* 0x64 - 16bit Primary, 0x68 - 16bit Secondary */

	/* FIXME: should these use address ? */
	/* Data read timing */
	pci_write_config_byte(pdev, 0x64 + 4*controller + (drive->dn&1),
		(cs5520_pio_clocks[pio].recovery << 4) |
		(cs5520_pio_clocks[pio].assert));
	/* Write command timing */
	pci_write_config_byte(pdev, 0x66 + 4*controller + (drive->dn&1),
		(cs5520_pio_clocks[pio].recovery << 4) |
		(cs5520_pio_clocks[pio].assert));
		
	/* Set the DMA enable/disable flag */
	reg = inb(hwif->dma_base + 0x02 + 8*controller);
	reg |= 1<<((drive->dn&1)+5);
	outb(reg, hwif->dma_base + 0x02 + 8*controller);
}

static void cs5520_set_dma_mode(ide_drive_t *drive, const u8 speed)
{
	printk(KERN_ERR "cs55x0: bad ide timing.\n");

	cs5520_set_pio_mode(drive, 0);
}

/*
 *	We wrap the DMA activate to set the vdma flag. This is needed
 *	so that the IDE DMA layer issues PIO not DMA commands over the
 *	DMA channel
 */
 
static int cs5520_dma_on(ide_drive_t *drive)
{
	/* ATAPI is harder so leave it for now */
	drive->vdma = 1;
	return 0;
}

static void __devinit init_hwif_cs5520(ide_hwif_t *hwif)
{
	hwif->set_pio_mode = &cs5520_set_pio_mode;
	hwif->set_dma_mode = &cs5520_set_dma_mode;

	if (hwif->dma_base == 0)
		return;

	hwif->ide_dma_on = &cs5520_dma_on;
}

#define DECLARE_CS_DEV(name_str)				\
	{							\
		.name		= name_str,			\
		.init_hwif	= init_hwif_cs5520,		\
		.host_flags	= IDE_HFLAG_ISA_PORTS |		\
				  IDE_HFLAG_CS5520 |		\
				  IDE_HFLAG_VDMA |		\
				  IDE_HFLAG_NO_ATAPI_DMA |	\
				  IDE_HFLAG_BOOTABLE,		\
		.pio_mask	= ATA_PIO4,			\
	}

static const struct ide_port_info cyrix_chipsets[] __devinitdata = {
	/* 0 */ DECLARE_CS_DEV("Cyrix 5510"),
	/* 1 */ DECLARE_CS_DEV("Cyrix 5520")
};

/*
 *	The 5510/5520 are a bit weird. They don't quite set up the way
 *	the PCI helper layer expects so we must do much of the set up 
 *	work longhand.
 */
 
static int __devinit cs5520_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	const struct ide_port_info *d = &cyrix_chipsets[id->driver_data];
	u8 idx[4] = { 0xff, 0xff, 0xff, 0xff };

	ide_setup_pci_noise(dev, d);

	/* We must not grab the entire device, it has 'ISA' space in its
	   BARS too and we will freak out other bits of the kernel */
	if (pci_enable_device_bars(dev, 1<<2)) {
		printk(KERN_WARNING "%s: Unable to enable 55x0.\n", d->name);
		return -ENODEV;
	}
	pci_set_master(dev);
	if (pci_set_dma_mask(dev, DMA_32BIT_MASK)) {
		printk(KERN_WARNING "cs5520: No suitable DMA available.\n");
		return -ENODEV;
	}

	/*
	 *	Now the chipset is configured we can let the core
	 *	do all the device setup for us
	 */

	ide_pci_setup_ports(dev, d, 14, &idx[0]);

	ide_device_add(idx);

	return 0;
}

static const struct pci_device_id cs5520_pci_tbl[] = {
	{ PCI_VDEVICE(CYRIX, PCI_DEVICE_ID_CYRIX_5510), 0 },
	{ PCI_VDEVICE(CYRIX, PCI_DEVICE_ID_CYRIX_5520), 1 },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, cs5520_pci_tbl);

static struct pci_driver driver = {
	.name		= "Cyrix_IDE",
	.id_table	= cs5520_pci_tbl,
	.probe		= cs5520_init_one,
};

static int __init cs5520_ide_init(void)
{
	return ide_pci_register_driver(&driver);
}

module_init(cs5520_ide_init);

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("PCI driver module for Cyrix 5510/5520 IDE");
MODULE_LICENSE("GPL");
