; MDVMM.COM - DOS loader for the modernDOS virtual machine monitor
;
; Checks the machine, takes a memory pool from XMS, copies the 32-bit
; core into it, switches to protected mode and jumps to the core. The
; core enables paging (through 'tramp' below), takes over interrupts and
; resumes this program at 'resumed' - now running in V86 mode. The loader
; then stays resident with only its small V86 stub area.

%include "bootinfo.inc"

        cpu     386
        bits    16
        org     100h

POOL_KB         equ 32768               ; default pool size
POOL_KB_MIN     equ 1024

start:
        jmp     main

; ----------------------------------------------------------------------
; resident part: V86-side stubs the monitor refers to
; ----------------------------------------------------------------------
; Breakpoints: the monitor recognizes a HLT at stub_bp + n (see bootinfo.inc)
stub_bp:
        times   BP_COUNT hlt
stub_psp        dw      0

; DPMI mode switch entry (returned by INT 2Fh AX=1687h)
stub_entry:
        push    ax
        push    bx
        mov     ah, 62h                 ; current PSP, for the client's ES
        int     21h
        mov     [cs:stub_psp], bx
        pop     bx
        pop     ax
        jmp     stub_bp + BP_DPMI_ENTRY

; DPMI 0305h real mode state save/restore: nothing to save
stub_retf:
        retf

        align   16
resident_end:

; ----------------------------------------------------------------------
; transient part
; ----------------------------------------------------------------------
main:
        mov     dx, msg_banner
        call    print

        call    check_386
        jc      err_386
        smsw    ax
        test    al, 1
        jz      .not_loaded
        mov     ax, MDVMM_MPX << 8      ; already running under MDVMM?
        int     2fh
        cmp     bx, MDVMM_SIGNATURE
        jne     err_pm
        jmp     command                 ; yes: MDVMM NEW ...
.not_loaded:

        ; XMS driver
        mov     ax, 4300h
        int     2fh
        cmp     al, 80h
        jne     err_xms
        push    es
        mov     ax, 4310h
        int     2fh
        mov     [xms_entry], bx
        mov     [xms_entry + 2], es
        pop     es

        ; pool size: default, or the largest free block if smaller
        mov     ah, 08h
        call    far [xms_entry]
        cmp     ax, POOL_KB_MIN + 4
        jb      err_mem
        mov     dx, POOL_KB + 4         ; +4K for page alignment
        cmp     ax, dx
        jae     .size_ok
        mov     dx, ax
.size_ok:
        mov     [pool_kb], dx
        mov     ah, 09h                 ; allocate EMB
        call    far [xms_entry]
        test    ax, ax
        jz      err_mem
        mov     [xms_handle], dx
        mov     ah, 0ch                 ; lock EMB -> DX:BX physical
        call    far [xms_entry]
        test    ax, ax
        jz      err_lock
        mov     [pool_raw], bx
        mov     [pool_raw + 2], dx
        mov     ah, 03h                 ; global enable A20, for good
        call    far [xms_entry]

        ; page aligned pool inside the EMB
        mov     eax, [pool_raw]
        add     eax, 0fffh
        and     eax, 0fffff000h
        mov     [bi + bootinfo.pool_phys], eax
        sub     eax, [pool_raw]
        mov     [align_skip], eax
        movzx   ecx, word [pool_kb]
        shl     ecx, 10
        sub     ecx, eax
        and     ecx, 0fffff000h
        mov     [bi + bootinfo.pool_size], ecx

        ; copy the core image to the start of the aligned pool
        mov     dword [xmove + 0], core_end - core_image
        mov     word [xmove + 4], 0             ; source: conventional
        mov     word [xmove + 6], core_image
        mov     [xmove + 8], cs
        mov     ax, [xms_handle]
        mov     [xmove + 10], ax
        mov     eax, [align_skip]
        mov     [xmove + 12], eax
        mov     si, xmove
        mov     ah, 0bh
        call    far [xms_entry]
        test    ax, ax
        jz      err_copy

        ; boot information for the core
        mov     dword [bi + bootinfo.magic], BOOTINFO_MAGIC
        mov     dword [bi + bootinfo.version], BOOTINFO_VERSION
        mov     dword [bi + bootinfo.core_size], core_end - core_image
        xor     ebx, ebx
        mov     bx, cs
        shl     ebx, 4                  ; ebx = linear base of this segment
        mov     eax, ebx
        add     eax, tramp
        mov     [bi + bootinfo.trampoline], eax
        mov     word [bi + bootinfo.ip], resumed
        mov     [bi + bootinfo.cs], cs
        mov     [bi + bootinfo.ss], ss
        mov     [bi + bootinfo.ds], ds
        mov     [bi + bootinfo.es], es
        mov     [bi + bootinfo.fs], fs
        mov     [bi + bootinfo.gs], gs
        mov     [bi + bootinfo.psp], cs
        mov     [bi + bootinfo.stub_seg], cs
        mov     word [bi + bootinfo.stub_bp], stub_bp
        mov     word [bi + bootinfo.stub_entry], stub_entry
        mov     word [bi + bootinfo.stub_psp], stub_psp
        mov     word [bi + bootinfo.stub_retf], stub_retf
        mov     eax, ebx
        add     eax, tmp_gdt
        mov     [tmp_gdtr + 2], eax
        mov     eax, ebx
        add     eax, pm32
        mov     [pm32_ptr], eax

        mov     dx, msg_switch
        call    print

        ; to protected mode
        cli
        mov     [bi + bootinfo.sp], sp
        pushfd
        pop     eax
        mov     [bi + bootinfo.eflags], eax
        o32 lgdt [tmp_gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     dword far [pm32_ptr]

        bits    32
pm32:
        mov     ax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        lea     esp, [ebx + pm_stack_top]
        add     ebx, bi                 ; ebx = linear address of bootinfo
        mov     eax, [ebx + bootinfo.pool_phys]
        jmp     eax                     ; core _start(eax = base, ebx = bi)

; called by the core: ESI = page directory, ECX = high entry point.
; Lives in identity mapped low memory so execution survives enabling paging.
tramp:
        mov     cr3, esi
        mov     eax, cr0
        or      eax, 80000000h
        mov     cr0, eax
        jmp     ecx

        bits    16
; ---- back in V86 mode, under the monitor ----
resumed:
        sti
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ax, MDVMM_MPX << 8
        int     2fh
        cmp     bx, MDVMM_SIGNATURE
        jne     err_resume
        mov     dx, msg_active
        call    print

        ; Everything below the end of this resident block is global to
        ; all DOS sessions, memory above is per session. Round the block
        ; up so that the boundary is page aligned. (The environment is
        ; kept: freeing it would leave a free hole in global memory.)
        mov     ax, cs
        add     ax, (resident_end - $$ + 100h + 15) >> 4
        add     ax, 0ffh
        xor     al, al                  ; next 4K boundary (256 paragraphs)
        mov     [reg_boundary], ax
        mov     bx, cs
        sub     ax, bx
        mov     [resident_paras], ax

        ; per session DOS data for the monitor
        push    es
        mov     ah, 34h                 ; InDOS flag
        int     21h
        mov     [reg_indos], bx
        mov     [reg_indos + 2], es
        mov     ah, 52h                 ; list of lists: CDS array
        int     21h
        mov     ax, [es:bx + 16h]
        mov     [reg_cds], ax
        mov     ax, [es:bx + 18h]
        mov     [reg_cds + 2], ax
        mov     al, [es:bx + 21h]       ; LASTDRIVE
        mov     ah, 58h                 ; bytes per CDS entry
        mul     ah
        mov     [reg_cds_size], ax
        ; enhanced mode startup broadcast: the kernel (and TSRs) answer
        ; with their instance data tables
        push    ds
        xor     bx, bx
        mov     es, bx
        mov     ds, bx
        xor     si, si
        xor     cx, cx
        xor     dx, dx                  ; enhanced mode
        mov     di, 030ah               ; "Windows 3.10"
        mov     ax, 1605h
        int     2fh
        pop     ds
        mov     [reg_sis], bx
        mov     [reg_sis + 2], es
        pop     es
        test    cx, cx
        jnz     .no_sessions            ; someone vetoed
        mov     ax, (MDVMM_MPX << 8) | 04h   ; register session data
        mov     dx, reg_block
        int     2fh
        jc      .no_sessions
        jmp     .tsr
.no_sessions:
        mov     dx, msg_nosess
        call    print
.tsr:
        mov     dx, [resident_paras]
        mov     ax, 3100h               ; stay resident
        int     21h

; ----------------------------------------------------------------------
; MDVMM NEW [command]: start a DOS session running COMSPEC (/C command)
command:
        mov     si, 81h
        call    skip_spaces
        mov     di, kw_new
        mov     cx, 3
.cmp:   lodsb
        and     al, 0dfh                ; upper case
        scasb
        jne     usage
        loop    .cmp
        call    skip_spaces
        ; command tail for COMSPEC: " /C <command>" or empty
        mov     di, tail + 1
        cmp     byte [si], 13
        je      .tail_done
        mov     ax, '/' | (' ' << 8)
        mov     byte [di], ' '
        inc     di
        mov     word [di], 'C' * 256 + '/'
        add     di, 2
        mov     byte [di], ' '
        inc     di
.copy:  lodsb
        cmp     al, 13
        je      .tail_done
        stosb
        cmp     di, tail + 126
        jb      .copy
.tail_done:
        mov     byte [di], 13
        mov     ax, di
        sub     ax, tail + 1
        mov     [tail], al
        call    find_comspec            ; DS:DX -> program
        jc      err_comspec

        ; release memory above the transient code, so the new session
        ; has room to run the shell
        mov     bx, (transient_end - $$ + 100h + 15) >> 4
        mov     ah, 4ah
        int     21h
        mov     sp, transient_end - 2

        mov     ax, (MDVMM_MPX << 8) | 02h   ; fork a new session
        int     2fh
        jc      err_new
        test    ax, ax
        jz      .child
        mov     dx, msg_started         ; parent: done
        call    print
        mov     ax, 4c00h
        int     21h
.child:                                 ; new session: run the shell
        mov     word [epb], 0           ; inherit environment
        mov     word [epb + 2], tail
        mov     [epb + 4], cs
        mov     word [epb + 6], 5ch
        mov     [epb + 8], cs
        mov     word [epb + 10], 6ch
        mov     [epb + 12], cs
        mov     dx, [comspec_ptr]
        push    ds
        mov     ds, [comspec_ptr + 2]
        mov     bx, epb
        push    cs
        pop     es
        mov     ax, 4b00h
        int     21h
        pop     ds
        mov     ax, (MDVMM_MPX << 8) | 03h   ; end this session
        int     2fh
        jmp     $                       ; not reached

skip_spaces:
        cmp     byte [si], ' '
        jne     .done
        inc     si
        jmp     skip_spaces
.done:  ret

; COMSPEC from the environment -> comspec_ptr; CF if not found
find_comspec:
        push    es
        mov     es, [2ch]
        xor     di, di
.var:   cmp     byte [es:di], 0
        je      .none
        mov     si, kw_comspec
        mov     bx, di
        mov     cx, 8
        repe    cmpsb
        je      .found
        mov     di, bx
.skip:  inc     di
        cmp     byte [es:di - 1], 0
        jne     .skip
        jmp     .var
.found: mov     [comspec_ptr], di
        mov     [comspec_ptr + 2], es
        pop     es
        clc
        ret
.none:  pop     es
        stc
        ret

usage:      mov     dx, msg_usage
            jmp     fail
err_comspec: mov    dx, msg_comspec
            jmp     fail
err_new:    mov     dx, msg_new
            jmp     fail

; ----------------------------------------------------------------------

; CF set if not at least a 386
check_386:
        pushf                           ; preserve the caller's flags
        pushf
        pop     ax
        mov     cx, ax
        and     ax, 0fffh
        push    ax
        popf
        pushf
        pop     ax
        and     ax, 0f000h
        cmp     ax, 0f000h
        je      .no                     ; 8086/80186
        or      cx, 7000h
        push    cx
        popf
        pushf
        pop     ax
        test    ax, 7000h
        jz      .no                     ; 80286
        popf
        clc
        ret
.no:    popf
        stc
        ret

print:
        mov     ah, 09h
        int     21h
        ret

err_386:    mov     dx, msg_386
            jmp     fail
err_pm:     mov     dx, msg_pm
            jmp     fail
err_xms:    mov     dx, msg_xms
            jmp     fail
err_mem:    mov     dx, msg_mem
            jmp     fail
err_lock:   mov     dx, msg_lock
            jmp     fail
err_copy:   mov     dx, msg_copy
            jmp     fail
err_resume: mov     dx, msg_resume
fail:
        call    print
        mov     ax, 4c01h
        int     21h

; ----------------------------------------------------------------------
msg_banner  db  'MDVMM - modernDOS virtual machine monitor', 13, 10, '$'
msg_switch  db  'Starting monitor...', 13, 10, '$'
msg_active  db  'MDVMM active: DOS is running in a virtual machine.', 13, 10, '$'
msg_386     db  'MDVMM needs a 386 or later CPU.', 13, 10, '$'
msg_pm      db  'CPU already in protected/V86 mode (MDVMM, EMM386 or a', 13, 10
            db  'similar memory manager is loaded).', 13, 10, '$'
msg_xms     db  'MDVMM needs an XMS driver (e.g. DEVICE=HIMEMX.EXE).', 13, 10, '$'
msg_mem     db  'Not enough free extended memory.', 13, 10, '$'
msg_lock    db  'Could not lock extended memory.', 13, 10, '$'
msg_copy    db  'Could not copy the monitor to extended memory.', 13, 10, '$'
msg_resume  db  'Monitor did not answer after start.', 13, 10, '$'
msg_nosess  db  'DOS kernel does not support sessions (no instance data).', 13, 10, '$'
msg_usage   db  'Usage: MDVMM            load the monitor', 13, 10
            db  '       MDVMM NEW [cmd]  start a DOS session (Ctrl+Alt+F1..F4 switch)', 13, 10, '$'
msg_comspec db  'COMSPEC not set.', 13, 10, '$'
msg_new     db  'Could not start a session.', 13, 10, '$'
msg_started db  'DOS session started.', 13, 10, '$'
kw_new      db  'NEW'
kw_comspec  db  'COMSPEC='

; session registration block (INT 2Fh AX=DE04h, DS:DX)
reg_block:
reg_sis         dd  0                   ; startup info chain (1605h)
reg_indos       dd  0                   ; InDOS flag
reg_cds         dd  0                   ; CDS array
reg_cds_size    dw  0
reg_boundary    dw  0                   ; first per-session paragraph
resident_paras  dw  0
comspec_ptr     dd  0
epb             times 14 db 0           ; EXEC parameter block
tail            times 128 db 0          ; command tail

        align   4
xms_entry   dd  0
xms_handle  dw  0
pool_kb     dw  0
pool_raw    dd  0
align_skip  dd  0
xmove       times 16 db 0               ; XMS move structure
pm32_ptr    dd  0
            dw  08h

        align   8
tmp_gdt     dq  0
            dw  0ffffh, 0               ; 08h: flat 32-bit code
            db  0, 9ah, 0cfh, 0
            dw  0ffffh, 0               ; 10h: flat 32-bit data
            db  0, 92h, 0cfh, 0
tmp_gdtr    dw  3 * 8 - 1
            dd  0

        align   4
bi          times bootinfo_size db 0

        align   16
            times 512 db 0
pm_stack_top:
transient_end:                          ; MDVMM NEW keeps memory up to here

        align   16
core_image:
        incbin  "core.bin"
        align   2, db 0
core_end:
