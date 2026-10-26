| ROM entry for the CPU benchmark: reset vectors, the overlay off, armcpu's
| fast RAM board placed at $200000, .data copied and .bss cleared, main().
	.section .vectors,"ax"
	.long	0x00280000		| SSP: top of the first 512 KB of fast RAM
	.long	_start
	.text
	.globl	_start
_start:
	move.w	#0x2700,%sr
#ifdef __mc68020__
	move.l	#0x80008001,%d0		| caches on, as Kickstart does: 68020 E, 68040 IE+DE
	movec	%d0,%cacr		| (UAE's JIT only runs with the instruction cache on)
#endif
	move.b	#3,0xBFE201		| CIA-A DDRA: OVL an output
	move.b	#0,0xBFE001		| PRA: overlay off
	move.b	#0x00,0xE8004A		| autoconfig: base low nibble
	move.b	#0x20,0xE80048		| base $200000
	lea	_sdata_rom,%a0
	lea	_sdata,%a1
	lea	_edata,%a2
1:	cmp.l	%a2,%a1
	bcc.s	2f
	move.l	(%a0)+,(%a1)+
	bra.s	1b
2:	lea	_ebss,%a2
3:	cmp.l	%a2,%a1
	bcc.s	4f
	clr.l	(%a1)+
	bra.s	3b
4:	jsr	main
5:	bra.s	5b
