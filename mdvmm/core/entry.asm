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

        ; PT0: identity map 0-4MB. Only the V86 address space (0-10FFFFh)
        ; is user accessible; the rest belongs to XMS and the monitor.
        lea     edi, [esi + 4096]
        mov     eax, 7
        mov     ecx, 1024
.pt0:   cmp     eax, 110000h
        jb      .user
        and     al, ~4
.user:  stosd
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
FRAME_SIZE equ 92
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
        push    ds
        push    es
        push    fs
        push    gs
        mov     ax, KDATA
        mov     ds, ax
        mov     es, ax
        cld
        push    esp
        call    trap_dispatch
        add     esp, 4
; Returning to a ring 3 protected mode client: IRET to a 16 bit stack
; segment only reloads SP, so ESP[31:16] would leak the monitor's stack
; address. "espfix": IRET from an alias stack segment whose ESP already
; has the client's upper 16 bits.
EFL_VM  equ 0x20000
ESPFIX_SEL equ 0x38
extern gdt_table
global trap_return
trap_return:
        test    dword [esp + 64], EFL_VM        ; frame eflags
        jnz     .plain
        test    byte [esp + 60], 3              ; frame cs: back to ring 3?
        jz      .plain
        mov     eax, [esp + 56]                 ; eip, cs, eflags, esp, ss
        mov     [espfix_buf], eax
        mov     eax, [esp + 60]
        mov     [espfix_buf + 4], eax
        mov     eax, [esp + 64]
        mov     [espfix_buf + 8], eax
        mov     eax, [esp + 68]
        mov     [espfix_buf + 12], eax
        mov     eax, [esp + 72]
        mov     [espfix_buf + 16], eax
        mov     ecx, [esp + 68]
        and     ecx, 0xffff0000                 ; new ESP: client's upper half
        mov     [espfix_ptr], ecx
        mov     eax, espfix_buf
        sub     eax, ecx                        ; segment base
        mov     [gdt_table + ESPFIX_SEL + 2], ax
        shr     eax, 16
        mov     [gdt_table + ESPFIX_SEL + 4], al
        mov     [gdt_table + ESPFIX_SEL + 7], ah
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        lss     esp, [cs:espfix_ptr]
        iretd
.plain:
        pop     gs
        pop     fs
        pop     es
        pop     ds
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

section .data
espfix_ptr:
        dd      0
        dw      ESPFIX_SEL

section .bss
align 16
espfix_buf:
        resb    32
        resb    16384
global kstack_top
kstack_top:
