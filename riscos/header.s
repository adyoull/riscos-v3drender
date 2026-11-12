@ V3DRender: the Raspberry Pi 4's GPU (V3D 4.2) for RISC OS, as Linux's v3d
@ driver drives it. Mesa's v3d driver, in the program, sends it work
@ through V3DRender_Ioctl; RISC3D lists it as a 3D driver.
@
@ PROVISIONAL: the SWI chunk (&C3D40) and the error block (&81D340) are
@ not allocated yet.
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
	.word	0xC3D40				@ SWI chunk (provisional)
	.word	cm_swi - module_start
	.word	swi_names - module_start
	.word	0				@ SWI decoding code
	.word	0				@ messages file
	.word	flags - module_start

title:	.asciz	"V3DRender"
help:	.asciz	"V3DRender\t0.01 (12 Nov 2026) Pi 4 GPU driver (V3D 4.2), after Linux's v3d"
swi_names:
	.asciz	"V3DRender\0Info\0Open\0Close\0Ioctl\0Reset\0"
	.align	2
commands:
	.asciz	"V3DRender"
	.align	2
	.word	cm_command_0 - module_start
	.word	0x00010000			@ 0-1 parameters
	.word	0
	.word	cmd_help - module_start
	.asciz	"V3DClock"
	.align	2
	.word	cm_command_1 - module_start
	.word	0x00010000
	.word	0
	.word	clock_help - module_start
	.word	0
cmd_help:
	.asciz	"*V3DRender shows the GPU, its memory and what it has done.\rSyntax: *V3DRender"
clock_help:
	.asciz	"*V3DClock shows or sets the GPU's clock, in MHz (0: the firmware's maximum).\rSyntax: *V3DClock [<MHz>]"
	.align	2
flags:	.word	1				@ 32-bit compatible

	.word	0				@ (no fast table)
@ Service_PreReset (&45), Service_WimpCloseDown (&53),
@ Service_ModulePostInit (&DA: RISC3D has arrived)
service:
	teq	r1, #0x53
	teqne	r1, #0x45
	teqne	r1, #0xDA
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

@ void call_hal (unsigned arg, void *routine, unsigned sb): a HAL routine
@ found with OS_Hardware 1, called directly (no SWI from the interrupt)
	.global	call_hal
call_hal:
	stmfd	sp!, {r4, r9, r12, lr}
	mov	r9, r2
	blx	r1
	ldmfd	sp!, {r4, r9, r12, pc}

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
	.word	0x81D34A
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
