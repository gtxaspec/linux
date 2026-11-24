/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_INGENIC_RMEM_H
#define __LINUX_SOC_INGENIC_RMEM_H

#include <linux/types.h>

int ingenic_rmem_region(phys_addr_t *base, phys_addr_t *size);

#endif
