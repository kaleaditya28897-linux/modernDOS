; MDVMM core entry, paging bootstrap and interrupt stubs.
;
; A loader jumps to _start in 32-bit protected mode, paging off, flat
; segments, with
;   EAX = physical address the core image was loaded at (4K aligned)
;   EBX = linear address of struct bootinfo (below 1MB)
; The core is linked at KBASE. Before paging is on, this code runs at its
; physical address and must be position independent.

%include "bootinfo.inc"

KBASE   equ 0xF0000000
KCODE   equ 0x08
KDATA   equ 0x10

bits 32

section .text.entry
global _start
extern __bss_start, __bss_end, core_main

_start:
        push    dword 2                 ; known EFLAGS: IF=0, IOPL=0, NT=0
        popfd
        cld
        mov     ebp, eax                ; ebp = physical base

        ; zero .bss (physical addresses)
        lea     edi, [ebp + (__bss_start - KBASE)]
        mov     ecx, __bss_end
        sub     ecx, __bss_start
        shr     ecx, 2
        xor     eax, eax
        rep     stosd

        ; page directory, identity PT for the first 4MB, PTs for the pool
        lea     esi, [ebp + (__bss_end - KBASE)]   ; esi = PD (phys)
        mov     edx, [ebx + bootinfo.pool_size]
        add     edx, 0x3FFFFF
        shr     edx, 22                 ; edx = number of pool PTs
        lea     ecx, [edx + 2]
        shl     ecx, 10
        mov     edi, esi
        xor     eax, eax
        rep     stosd

        ; PT0: identity map 0-4MB, user accessible (V86 runs at CPL 3)
        lea     edi, [esi + 4096]
        mov     eax, 7
        mov     ecx, 1024
.pt0:   stosd
        add     eax, 4096
        loop    .pt0
        lea     eax, [esi + 4096 + 7]
        mov     [esi], eax

        ; pool PTs: KBASE.. -> pool, supervisor only
        lea     edi, [esi + 8192]
        mov     eax, ebp
        or      eax, 3
        mov     ecx, [ebx + bootinfo.pool_size]
        shr     ecx, 12
.ptk:   stosd
        add     eax, 4096
        loop    .ptk
        lea     edi, [esi + (KBASE >> 22) * 4]
        lea     eax, [esi + 8192 + 3]
        mov     ecx, edx
.pdk:   stosd
        add     eax, 4096
        loop    .pdk

        ; recursive mapping: PTs at 0xFFC00000, PD at 0xFFFFF000
        lea     eax, [esi + 3]
        mov     [esi + 1023 * 4], eax

        ; first free physical page after the tables
        lea     edi, [edx + 2]
        shl     edi, 12
        add     edi, esi

        ; turn paging on from identity mapped low memory, continue high
        mov     ecx, high_entry
        mov     eax, [ebx + bootinfo.trampoline]
        jmp     eax

high_entry:
        mov     esp, kstack_top
        push    edi                     ; first free physical page
        push    ebx                     ; struct bootinfo *
        call    core_main
.hang:  cli
        hlt
        jmp     .hang

section .text

; void gdt_load(void *gdtr)
global gdt_load
gdt_load:
        mov     eax, [esp + 4]
        lgdt    [eax]
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        jmp     KCODE:.flush
.flush: ret

; void enter_frame(const struct frame *f) - resume a trap frame (e.g. enter
; V86). The frame is copied to the top of the kernel stack, the C caller's
; stack is abandoned.
FRAME_SIZE equ 76
global enter_frame
enter_frame:
        mov     esi, [esp + 4]
        mov     edi, kstack_top - FRAME_SIZE
        mov     ecx, FRAME_SIZE / 4
        cld
        rep     movsd
        mov     esp, kstack_top - FRAME_SIZE
        jmp     trap_return

; Interrupt stubs: push a dummy error code where the CPU does not, then
; the vector number. Frame layout matches struct frame in core.h.
%assign i 0
%rep 256
isr%[i]:
 %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
 %else
        push    dword 0
 %endif
        push    dword i
        jmp     isr_common
%assign i i+1
%endrep

extern trap_dispatch
isr_common:
        pushad
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        cld
        push    esp
        call    trap_dispatch
        add     esp, 4
global trap_return
trap_return:
        popad
        add     esp, 8                  ; vector, error code
        iretd

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
        dd      isr%[i]
%assign i i+1
%endrep

section .bss
align 16
        resb    16384
global kstack_top
kstack_top:
