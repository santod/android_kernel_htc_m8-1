#ifndef __ASM_SH_HITACHI_SE_H
#define __ASM_SH_HITACHI_SE_H

/*
 * linux/include/asm-sh/hitachi_se.h
 *
 * Copyright (C) 2000  Kazumoto Kojima
 *
 * Hitachi SolutionEngine support
 */


#define PA_ROM		0x00000000	
#define PA_ROM_SIZE	0x00400000	
#define PA_FROM		0x01000000	
#define PA_FROM_SIZE	0x00400000	
#define PA_EXT1		0x04000000
#define PA_EXT1_SIZE	0x04000000
#define PA_EXT2		0x08000000
#define PA_EXT2_SIZE	0x04000000
#define PA_SDRAM	0x0c000000
#define PA_SDRAM_SIZE	0x04000000

#define PA_EXT4		0x12000000
#define PA_EXT4_SIZE	0x02000000
#define PA_EXT5		0x14000000
#define PA_EXT5_SIZE	0x04000000
#define PA_PCIC		0x18000000	

#define PA_83902	0xb0000000	
#define PA_83902_IF	0xb0040000	
#define PA_83902_RST	0xb0080000	

#define PA_SUPERIO	0xb0400000	
#define PA_DIPSW0	0xb0800000	
#define PA_DIPSW1	0xb0800002	
#define PA_LED		0xb0c00000	
#if defined(CONFIG_CPU_SUBTYPE_SH7705)
#define PA_BCR		0xb0e00000
#else
#define PA_BCR		0xb1400000	
#endif

#define PA_MRSHPC	0xb83fffe0	
#define PA_MRSHPC_MW1	0xb8400000	
#define PA_MRSHPC_MW2	0xb8500000	
#define PA_MRSHPC_IO	0xb8600000	
#define MRSHPC_OPTION   (PA_MRSHPC + 6)
#define MRSHPC_CSR      (PA_MRSHPC + 8)
#define MRSHPC_ISR      (PA_MRSHPC + 10)
#define MRSHPC_ICR      (PA_MRSHPC + 12)
#define MRSHPC_CPWCR    (PA_MRSHPC + 14)
#define MRSHPC_MW0CR1   (PA_MRSHPC + 16)
#define MRSHPC_MW1CR1   (PA_MRSHPC + 18)
#define MRSHPC_IOWCR1   (PA_MRSHPC + 20)
#define MRSHPC_MW0CR2   (PA_MRSHPC + 22)
#define MRSHPC_MW1CR2   (PA_MRSHPC + 24)
#define MRSHPC_IOWCR2   (PA_MRSHPC + 26)
#define MRSHPC_CDCR     (PA_MRSHPC + 28)
#define MRSHPC_PCIC_INFO (PA_MRSHPC + 30)

#define BCR_ILCRA	(PA_BCR + 0)
#define BCR_ILCRB	(PA_BCR + 2)
#define BCR_ILCRC	(PA_BCR + 4)
#define BCR_ILCRD	(PA_BCR + 6)
#define BCR_ILCRE	(PA_BCR + 8)
#define BCR_ILCRF	(PA_BCR + 10)
#define BCR_ILCRG	(PA_BCR + 12)

#if defined(CONFIG_CPU_SUBTYPE_SH7709)
#define INTC_IRR0       0xa4000004UL
#define INTC_IRR1       0xa4000006UL
#define INTC_IRR2       0xa4000008UL

#define INTC_ICR0       0xfffffee0UL
#define INTC_ICR1       0xa4000010UL
#define INTC_ICR2       0xa4000012UL
#define INTC_INTER      0xa4000014UL

#define INTC_IPRC       0xa4000016UL
#define INTC_IPRD       0xa4000018UL
#define INTC_IPRE       0xa400001aUL

#define IRQ0_IRQ        32
#define IRQ1_IRQ        33
#endif

#if defined(CONFIG_CPU_SUBTYPE_SH7705)
#define IRQ_STNIC	12
#define IRQ_CFCARD	14
#else
#define IRQ_STNIC	10
#define IRQ_CFCARD	7
#endif

#define SH_ETH0_BASE 0xA7000000
#define SH_ETH1_BASE 0xA7000400
#if defined(CONFIG_CPU_SUBTYPE_SH7710)
# define PHY_ID 0x00
#elif defined(CONFIG_CPU_SUBTYPE_SH7712)
# define PHY_ID 0x01
#endif
#define SH_ETH0_IRQ	80
#define SH_ETH1_IRQ	81
#define SH_TSU_IRQ	82

void init_se_IRQ(void);

#define __IO_PREFIX	se
#include <asm/io_generic.h>

#endif  
