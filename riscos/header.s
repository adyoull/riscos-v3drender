@ V3DRender: the Raspberry Pi 4's GPU (V3D 4.2) for RISC OS, as Linux's v3d
@ driver drives it. Mesa's v3d driver, in the program, sends it work
@ through V3DRender_Ioctl; RISC3D lists it as a 3D driver.
@
@ SWI chunk &5AFC0 and error block &823E00, allocated by RISC OS Open
@ (18 November 2026; 0.15 and before used provisional &C3D40 and &81D340).
@
@ SPDX-License-Identifier: GPL-2.0-or-later
@ Copyright (c) 2026 Andrew Youll

	.syntax	unified
	.arm
	.section .text.header, "ax"
	.global	module_start
module_start:
	.word	0				@ start
	.word	cm_init - module_start
	.word	cm_final - module_start
	.word	service - module_start
	.word	title - module_start
	.word	help - module_start
	.word	commands - module_start
	.word	0x5AFC0				@ SWI chunk (allocated)
	.word	cm_swi - module_start
	.word	swi_names - module_start
	.word	0				@ SWI decoding code
	.word	0				@ messages file
	.word	flags - module_start

title:	.asciz	"V3DRender"
help:	.asciz	"V3DRender\t0.29 (23 Nov 2026) Pi 4 GPU driver (V3D 4.2), after Linux's v3d"
swi_names:
	.asciz	"V3DRender\0Info\0Open\0Close\0Ioctl\0Reset\0Debug\0"
	.align	2
commands:
	.asciz	"V3DRender"
	.align	2
	.word	cm_command_0 - module_start
	.word	0x00070000			@ 0-7 parameters
	.word	0
	.word	cmd_help - module_start
	.asciz	"V3DClock"
	.align	2
	.word	cm_command_1 - module_start
	.word	0x00010000
	.word	0
	.word	clock_help - module_start
	.asciz	"V3DDump"
	.align	2
	.word	cm_command_2 - module_start
	.word	0x00010001			@ 1 parameter
	.word	0
	.word	dump_help - module_start
	.asciz	"V3DPower"
	.align	2
	.word	cm_command_3 - module_start
	.word	0x00010000			@ 0-1 parameters
	.word	0
	.word	power_help - module_start
	.asciz	"V3DReset"
	.align	2
	.word	cm_command_4 - module_start
	.word	0x00010000			@ 0-1 parameters
	.word	0
	.word	reset_help - module_start
	.word	0
cmd_help:
	.asciz	"*V3DRender shows the GPU, its memory and what it has done. *V3DRender 2D on|async|off turns the GPU's desktop copies and fills (window moves, scrolls, plain rectangles) on, on without waiting for each, or off; *V3DRender 2D fills on|off, the fills alone; *V3DRender 2D alongside on|off, also while a program has the GPU open (off: the CPU does them then); *V3DRender 2D limits [copy-bytes copy-rows fill-bytes fill-rows fill-bytes-a-tile], below which the CPU does them (none: the defaults, 1024 8 4096 16 0; V3DDriver's were 2048 16 16384 32 4500). *V3DRender pool <MB> sets how much freed GPU memory is kept for programs' next buffers (default 16; 0 keeps none).\rSyntax: *V3DRender [pool <MB>|2D on|async|off|fills on|off|alongside on|off|limits [5 numbers]|zero]"
dump_help:
	.asciz	"*V3DDump saves what the GPU's buffers held when a job first hung since V3DRender was loaded, with its registers, for V3DRender's tools/v3ddump.py.\rSyntax: *V3DDump <file>"
reset_help:
	.asciz	"*V3DReset recovers the GPU after a program failed: every job running or waiting is ended (as failed) and the GPU is reset. *V3DReset all also closes every V3DRender file, with its buffers, for programs that stopped without closing theirs (any program still using the GPU must start again).\rSyntax: *V3DReset [all]"
power_help:
	.asciz	"*V3DPower shows the V3D's power: the firmware's power domain and PM_GRAFX. *V3DPower cycle has the firmware turn the domain off and on, then resets the GPU (only while nothing uses it).\rSyntax: *V3DPower [cycle]"
clock_help:
	.asciz	"*V3DClock shows or sets the GPU's clock, in MHz (0: the firmware's maximum).\rSyntax: *V3DClock [<MHz>]"
	.align	2
flags:	.word	1				@ 32-bit compatible

	.word	0				@ (no fast table)
@ Service_PreReset (&45), Service_WimpCloseDown (&53),
@ Service_ModulePostInit (&DA: RISC3D has arrived), and for the 2D copies
@ (0.17) Service_ModeChange (&46) and Service_PreModeChange (&4D)
service:
	teq	r1, #0x53
	teqne	r1, #0x45
	teqne	r1, #0xDA
	teqne	r1, #0x46
	teqne	r1, #0x4D
	teqne	r1, #0x8E			@ (0.23) Service_PagesUnsafe
	teqne	r1, #0x8F			@ Service_PagesSafe
	teqne	r1, #0xE3			@ Service_PagesUnsafe64
	teqne	r1, #0xE4			@ Service_PagesSafe64
	movne	pc, lr
	b	cm_service

@ The interrupt (device 10): IRQ mode, interrupts off, r12 = our private
@ word. The vector is claimed unshared and the interrupt always taken, so
@ the return is through lr (PRM: OS_ClaimDeviceVector).
	.global	irq_entry
irq_entry:
	stmfd	sp!, {r0-r5, r12, lr}
	mov	r4, sp
	bic	sp, sp, #7			@ C wants an 8-byte aligned stack
	bl	irq_c
	mov	sp, r4
	ldmfd	sp!, {r0-r5, r12, pc}

@ (0.17) GraphicsV (vector 42): r0-r9 the call, r4 = reason | head << 16 |
@ driver << 24, r12 = our private word. gfxv_c gets the registers in a
@ block; 0 passes the call on (return through lr), 1 claims it (return
@ to the address the kernel stacked, with r4 = 0 from the block).
	.global	gfxv_entry
gfxv_entry:
	stmfd	sp!, {r0-r10, lr}
	mov	r10, sp
	bic	sp, sp, #7			@ C wants an 8-byte aligned stack
	mov	r0, r10
	bl	gfxv_c
	mov	sp, r10
	cmp	r0, #0
	ldmfd	sp!, {r0-r10, lr}
	moveq	pc, lr				@ not ours: on to the next claimant
	ldmfd	sp!, {pc}			@ claimed

@ (0.26) A transient callback (OS_AddCallBack): SVC mode, every register
@ preserved, back by MOV PC, R14. Interrupts on while callback_c runs
@ (it may claim and free RISC OS memory); the PSR as it was after.
	.global	callback_entry
callback_entry:
	stmfd	sp!, {r0-r12, lr}
	mrs	r4, cpsr
	mov	r5, sp
	bic	sp, sp, #7			@ C wants an 8-byte aligned stack
	cpsie	i
	bl	callback_c
	msr	cpsr_cxsf, r4
	mov	sp, r5
	ldmfd	sp!, {r0-r12, pc}

@ void call_hal (unsigned arg, void *routine, unsigned sb): a HAL routine
@ found with OS_Hardware 1, called directly (no SWI from the interrupt)
	.global	call_hal
call_hal:
	stmfd	sp!, {r4, r9, r12, lr}
	mov	r9, r2
	blx	r1
	ldmfd	sp!, {r4, r9, r12, pc}

@ (0.11) The CPU's data caches by address, to the point of coherency
@ (the L2 too, on the Cortex-A72): DCCMVAC cleans, DCIMVAC invalidates.
@ r0 = start, r1 = end; the line size from CTR.DminLine.
	.global	cache_clean_range
cache_clean_range:
	mrc	p15, 0, r2, c0, c0, 1		@ CTR
	ubfx	r2, r2, #16, #4
	mov	r3, #4
	lsl	r3, r3, r2			@ bytes a line
	sub	r2, r3, #1
	bic	r0, r0, r2
	dsb	sy
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c10, 1		@ DCCMVAC
	add	r0, r0, r3
	b	1b
2:	dsb	sy
	mov	pc, lr

	.global	cache_inval_range
cache_inval_range:
	mrc	p15, 0, r2, c0, c0, 1
	ubfx	r2, r2, #16, #4
	mov	r3, #4
	lsl	r3, r3, r2
	sub	r2, r3, #1
	bic	r0, r0, r2
	dsb	sy
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c14, 1		@ DCCIMVAC (0.14; 0.11-0.13: DCIMVAC)
	add	r0, r0, r3
	b	1b
2:	dsb	sy
	mov	pc, lr

@ The buffer areas' handler: a PMP is never grown or shrunk by
@ OS_ChangeDynamicArea (r0 = 0 PreGrow, 2 PreShrink are refused)
	.global	pmp_handler
pmp_handler:
	cmp	r0, #0
	cmpne	r0, #2
	beq	1f
	msr	APSR_nzcvq, #0
	mov	pc, lr
1:	adr	r0, pmp_error
	msr	APSR_nzcvq, #0x10000000
	mov	pc, lr
pmp_error:
	.word	0x823E0A
	.asciz	"V3DRender's memory can't be resized"
	.align	2

@ RISC3D's call to a driver: r0 = 0 open, 1 close, 2 sync
	.global	driver_entry
driver_entry:
	stmfd	sp!, {r0-r9, lr}
	mov	r0, sp
	bl	driver_call
	cmp	r0, #0
	bne	1f
	ldmfd	sp!, {r0-r9, lr}
	msr	APSR_nzcvq, #0
	mov	pc, lr
1:	add	sp, sp, #4			@ r0 = the error
	ldmfd	sp!, {r1-r9, lr}
	msr	APSR_nzcvq, #0x10000000
	mov	pc, lr

	.include "cmodule.inc"
	CM_COMMAND 0
	CM_COMMAND 1
	CM_COMMAND 2
	CM_COMMAND 3
	CM_COMMAND 4
