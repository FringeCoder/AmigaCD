// SPDX-License-Identifier: GPL-2.0
/*
 * armwin -- the FPGA's 68k window and the chip RAM shadow, mapped as Device
 * memory (ARM-CPU plan, step 6; AmigaCD docs/arm-cpu-plan.md).
 *
 * /dev/mem maps every address above Linux's RAM strongly ordered, so each
 * store armuae makes to chip RAM waits for its answer from the FPGA before
 * the next instruction runs. Device memory keeps the order of accesses to
 * the same peripheral but lets the core post a store and carry on, which is
 * what a 68k write to chip RAM needs.
 *
 * mmap() of /dev/armwin takes the physical address as its offset, as
 * /dev/mem does, but only inside two ranges:
 *   0xC0000000-0xC0FFFFFF  the core's flat 68k window (HPS-to-FPGA bridge)
 *   0x3B000000-0x3B1FFFFF  the chip RAM shadow in DDR3 (rtl/chip_mirror.v)
 * Everything else is refused. Loads stay where they are: armuae still reads
 * the shadow through a strongly-ordered /dev/mem mapping, because a Device
 * load to DDR3 is not ordered behind a Device load through the bridge, and
 * the read it waits on (DMACONR, INTREQR) is how it knows a DMA has ended.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#define WIN_BASE   0xC0000000UL
#define WIN_SIZE   0x01000000UL
#define SHD_BASE   0x3B000000UL
#define SHD_SIZE   0x00200000UL

static int inside(unsigned long pa, unsigned long len, unsigned long base, unsigned long size)
{
	return pa >= base && len <= size && pa - base <= size - len;
}

static int armwin_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long pa = vma->vm_pgoff << PAGE_SHIFT;

	if (!inside(pa, len, WIN_BASE, WIN_SIZE) && !inside(pa, len, SHD_BASE, SHD_SIZE))
		return -EINVAL;
	vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, len, vma->vm_page_prot);
}

static const struct file_operations armwin_fops = {
	.owner = THIS_MODULE,
	.mmap  = armwin_mmap,
};

static struct miscdevice armwin_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "armwin",
	.fops  = &armwin_fops,
	.mode  = 0600,
};

module_misc_device(armwin_dev);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AmigaCD ARM CPU: the FPGA 68k window and chip RAM shadow as Device memory");
