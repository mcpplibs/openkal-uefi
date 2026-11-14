/* The four functions a compiler calls for a block of memory (memcpy, memmove, memset, memcmp),
 * which a freestanding implementation may require of the C library (openkal-linux states the
 * same exception). This platform has no C library, so it supplies them. The Microsoft x64
 * convention: arguments in RCX, RDX, R8; RDI and RSI are the callee's to preserve. */
	.text

	.globl memset
	.def memset; .scl 2; .type 32; .endef
memset:                              /* (dst, c, n) -> dst */
	push %rdi
	mov %rcx, %r9
	mov %rcx, %rdi
	mov %edx, %eax
	mov %r8, %rcx
	rep stosb
	mov %r9, %rax
	pop %rdi
	ret

	.globl memcpy
	.def memcpy; .scl 2; .type 32; .endef
memcpy:                              /* (dst, src, n) -> dst */
	push %rdi
	push %rsi
	mov %rcx, %r9
	mov %rcx, %rdi
	mov %rdx, %rsi
	mov %r8, %rcx
	rep movsb
	mov %r9, %rax
	pop %rsi
	pop %rdi
	ret

	.globl memmove
	.def memmove; .scl 2; .type 32; .endef
memmove:                             /* (dst, src, n) -> dst; the blocks may overlap */
	push %rdi
	push %rsi
	mov %rcx, %r9
	mov %rcx, %rdi
	mov %rdx, %rsi
	mov %r8, %rcx
	cmp %rsi, %rdi
	jbe 1f                           /* the destination is below: forwards */
	lea -1(%rsi,%rcx), %rsi          /* otherwise backwards, from the last byte */
	lea -1(%rdi,%rcx), %rdi
	std
	rep movsb
	cld
	jmp 2f
1:	rep movsb
2:	mov %r9, %rax
	pop %rsi
	pop %rdi
	ret

	.globl memcmp
	.def memcmp; .scl 2; .type 32; .endef
memcmp:                              /* (a, b, n) -> <0, 0, >0 */
	xor %eax, %eax
	test %r8, %r8
	jz 2f
1:	movzbl (%rcx), %eax
	movzbl (%rdx), %r9d
	sub %r9d, %eax
	jnz 2f
	inc %rcx
	inc %rdx
	dec %r8
	jnz 1b
2:	ret
