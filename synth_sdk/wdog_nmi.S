/* Watchdog NMI (wdog.c): the record at 0x03FFFE00, then a watchdog reset. Never returns. Words: 0 magic "HANG",
 * 1 RETN (where the core was), 2 RETI, 3 RETX, 4 RETS, 5 IPEND, 6 ILAT, 7 SEQSTAT, 8 SP, 9 FP, 10 USP, 11 -,
 * 12..43 the 32 words at SP (if SP lies in SDRAM or L1 scratch). The record is flushed from the data cache. */
	.text
	.align 4
	.global _wd_nmi
	.type _wd_nmi, STT_FUNC
_wd_nmi:
	P0.L = 0xFE00; P0.H = 0x03FF;
	R0 = RETN; [P0 + 4] = R0;
	R0 = RETI; [P0 + 8] = R0;
	R0 = RETX; [P0 + 12] = R0;
	R0 = RETS; [P0 + 16] = R0;
	P1.L = 0x2108; P1.H = 0xFFE0;
	R0 = [P1]; [P0 + 20] = R0;                    /* IPEND */
	R0 = [P1 + 4]; [P0 + 24] = R0;                /* ILAT */
	R0 = SEQSTAT; [P0 + 28] = R0;
	R0 = SP; [P0 + 32] = R0;
	R0 = FP; [P0 + 36] = R0;
	R0 = USP; [P0 + 40] = R0;
	R0 = 0; [P0 + 44] = R0;
	R1 = SP;                                      /* the stack copy: SDRAM 0x1000 .. 0x03FFFC00 or L1 scratch */
	R2 = 3;
	R2 = R1 & R2;
	CC = R2 == 0;
	if !CC jump .Lmagic;                          /* a misaligned SP would fault in here (no reset): no copy */
	R2.L = 0x1000; R2.H = 0;
	CC = R1 < R2 (IU);
	if CC jump .Lscr;
	R2.L = 0xFC00; R2.H = 0x03FF;
	CC = R1 < R2 (IU);
	if CC jump .Lcopy;
.Lscr:
	R2.L = 0x0000; R2.H = 0xFFB0;
	CC = R1 < R2 (IU);
	if CC jump .Lmagic;
	R2.L = 0x0F80; R2.H = 0xFFB0;
	CC = R1 < R2 (IU);
	if !CC jump .Lmagic;
.Lcopy:
	P2 = R1;
	P1 = P0;
	P1 += 48;
	P5 = 32;
	LSETUP (.Lc0, .Lc1) LC0 = P5;
.Lc0:	R0 = [P2++];
.Lc1:	[P1++] = R0;
.Lmagic:
	R0.L = 0x4E47; R0.H = 0x4841;
	[P0] = R0;
	P1 = P0;
	P5 = 8;                                       /* 8 x 32 bytes out of the data cache */
	LSETUP (.Lf0, .Lf0) LC0 = P5;
.Lf0:	FLUSH [P1++];
	SSYNC;
#ifdef WD_NORESET                                     /* op1emu test: stay, the record can be read */
.Lstay:	jump .Lstay;
#endif
	P1.L = 0x0200; P1.H = 0xFFC0;                 /* watchdog reset, as TE's 0x01A88794 */
	R0 = 0x0AD0 (Z); W[P1] = R0;
	SSYNC;
	R0 = 1; [P1 + 4] = R0;
	R0 = 0x0FF0 (Z); W[P1] = R0;
	SSYNC;
.Lw:	jump .Lw;
	.size _wd_nmi, .-_wd_nmi
