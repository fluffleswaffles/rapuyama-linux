/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef RYISI_SIM_BRIDGE_H
#define RYISI_SIM_BRIDGE_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct isi_sim_reg{__u32 offset,size,value,write;};
struct isi_sim_pmic{__u32 device,reg,value,write;};
struct isi_sim_irq{__u32 number,enable;};
struct isi_sim_adc{__u32 channel,value;};

#define ISI_SIM_REG _IOWR('S',0,struct isi_sim_reg)
#define ISI_SIM_PMIC _IOWR('S',1,struct isi_sim_pmic)
#define ISI_SIM_IRQ _IOW('S',2,struct isi_sim_irq)
#define ISI_SIM_ADC _IOWR('S',3,struct isi_sim_adc)
#endif