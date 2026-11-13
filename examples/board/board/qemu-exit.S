/* THE BOARD'S EXIT: a status leaves the machine through QEMU's `isa-debug-exit' device (port 0xf4),
 * whose write ends the emulator with (status << 1) | 1. openkal-uefi calls this before Exit
 * (okuefi_board_exit); its own definition does nothing.
 *
 * Only under QEMU. The processor states a hypervisor in CPUID leaf 1 (ECX bit 31) and names it in
 * leaf 0x40000000 ("TCGTCGTCGTCG" for QEMU's translator, "KVMKVMKVM" under KVM); on a machine
 * that is neither, nothing is written and the status leaves through Exit alone. The Microsoft x64
 * convention: the status arrives in ECX. */
	.text
	.globl okuefi_board_exit
	.def okuefi_board_exit; .scl 2; .type 32; .endef
okuefi_board_exit:
	push %rbx
	mov %ecx, %r8d
	mov $1, %eax
	cpuid
	bt $31, %ecx
	jnc 1f
	mov $0x40000000, %eax
	cpuid
	cmp $0x54474354, %ebx        /* "TCGT" */
	je 2f
	cmp $0x4b4d564b, %ebx        /* "KVMK" */
	jne 1f
2:	mov %r8d, %eax
	and $0xff, %eax
	mov $0xf4, %dx
	out %eax, %dx
1:	pop %rbx
	ret
