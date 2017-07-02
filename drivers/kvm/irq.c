/*
 * irq.c: API for in kernel interrupt controller
 * Copyright (c) 2007, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 * Authors:
 *   Yaozu (Eddie) Dong <Eddie.dong@intel.com>
 *
 */

#include <linux/module.h>

#include "kvm.h"
#include "irq.h"

/*
 * check if there is pending interrupt without
 * intack.
 */
int kvm_cpu_has_interrupt(struct kvm_vcpu *v)
{
	struct kvm_pic *s;

	if (kvm_apic_has_interrupt(v) == -1) {	/* LAPIC */
		if (kvm_apic_accept_pic_intr(v)) {
			s = pic_irqchip(v->kvm);	/* PIC */
			return s->output;
		} else
			return 0;
	}
	return 1;
}
EXPORT_SYMBOL_GPL(kvm_cpu_has_interrupt);

/*
 * Read pending interrupt vector and intack.
 */
int kvm_cpu_get_interrupt(struct kvm_vcpu *v)
{
	struct kvm_pic *s;
	int vector;

	vector = kvm_get_apic_interrupt(v);	/* APIC */
	if (vector == -1) {
		if (kvm_apic_accept_pic_intr(v)) {
			s = pic_irqchip(v->kvm);
			s->output = 0;		/* PIC */
			vector = kvm_pic_read_irq(s);
		}
	}
	return vector;
}
EXPORT_SYMBOL_GPL(kvm_cpu_get_interrupt);

static void vcpu_kick_intr(void *info)
{
#ifdef DEBUG
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *)info;
	printk(KERN_DEBUG "vcpu_kick_intr %p \n", vcpu);
#endif
}

void kvm_vcpu_kick(struct kvm_vcpu *vcpu)
{
	int ipi_pcpu = vcpu->cpu;

	if (waitqueue_active(&vcpu->wq)) {
		wake_up_interruptible(&vcpu->wq);
		++vcpu->stat.halt_wakeup;
	}
	if (vcpu->guest_mode)
		smp_call_function_single(ipi_pcpu, vcpu_kick_intr, vcpu, 0, 0);
}

void kvm_inject_pending_timer_irqs(struct kvm_vcpu *vcpu)
{
	kvm_inject_apic_timer_irqs(vcpu);
	/* TODO: PIT, RTC etc. */
}
EXPORT_SYMBOL_GPL(kvm_inject_pending_timer_irqs);

void kvm_timer_intr_post(struct kvm_vcpu *vcpu, int vec)
{
	kvm_apic_timer_intr_post(vcpu, vec);
	/* TODO: PIT, RTC etc. */
}
EXPORT_SYMBOL_GPL(kvm_timer_intr_post);
