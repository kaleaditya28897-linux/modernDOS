; MDVMM UEFI loader: the embedded core and the hand-over to it.
;
; void enter_core(uint64_t low, uint64_t bootinfo, uint64_t pool)
;   low      - a page below 640K (identity mapped by the firmware)
;   bootinfo - struct bootinfo in that page
;   pool     - physical base of the pool, the core is copied there
; Copies the 32-bit hand-over code into the low page, leaves long mode
; (compatibility mode, paging off, EFER.LME off) and jumps to the core's
; _start with EAX = pool, EBX = bootinfo, as the DOS loader does. The
; core turns on its own paging through 'tramp', which must be identity
; mapped: it stays in the low page.

bits 64
section .text

global enter_core
enter_core:
        cli
        mov     rbp, rdi                ; low page
        mov     r8, rsi                 ; bootinfo
        mov     r9, rdx                 ; pool
        lea     rsi, [rel blob]
        mov     rcx, blob_end - blob
        cld
        rep     movsb                   ; rdi = low page
        lea     rax, [rbp + (gdt - blob)]
        mov     [rbp + (gdtr - blob) + 2], rax
        lgdt    [rbp + (gdtr - blob)]
        lea     rax, [rbp + (tramp - blob)]
        mov     [r8 + 20], eax          ; bootinfo.trampoline
        mov     ebx, r8d
        mov     edi, r9d
        push    qword 08h               ; 32-bit code segment
        lea     rax, [rbp + (pm32 - blob)]
        push    rax
        db      48h, 0cbh               ; retfq: to compatibility mode

; ---- copied to the low page, position independent ----
blob:
bits 32
pm32:
        mov     ax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     eax, cr0                ; paging off: leaves long mode
        and     eax, 7ffeffffh          ; (also clear WP, as from DOS)
        mov     cr0, eax
        mov     ecx, 0c0000080h         ; EFER.LME off
        rdmsr
        and     eax, ~100h
        wrmsr
        xor     eax, eax
        mov     cr4, eax
        lea     esp, [ebp + 1000h]
        mov     eax, edi
        jmp     edi                     ; core _start(eax = base, ebx = bi)

; called by the core: ESI = page directory, ECX = high entry point
tramp:
        mov     cr3, esi
        mov     eax, cr0
        or      eax, 80000000h
        mov     cr0, eax
        jmp     ecx

        align   8
gdt:    dq      0
        dq      00cf9a000000ffffh       ; 08h: flat 32-bit code
        dq      00cf92000000ffffh       ; 10h: flat 32-bit data
gdtr:   dw      3 * 8 - 1
        dq      0
blob_end:

section .rodata
global core_image, core_image_end
        align   16
core_image:
        incbin  "core.bin"
core_image_end:
