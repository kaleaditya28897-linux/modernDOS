; SSHSLOT.COM - job slots: DOS sessions that run commands for SSHD
;
;   SSHSLOT [n]      start n job slots (1..3, default 2)
;
; Forks n MDVMM sessions (before SSHD starts: a session can not be forked
; from a DPMI program). Each one waits for a job from the monitor
; (INT 2Fh AX=DE0Ah), runs it with COMSPEC (an empty command: an
; interactive shell, else "/C command"), reports the exit code (DE0Bh) and
; waits again. SSHD starts jobs (DE0Ch), reads their screens (DE08h) and
; types into them (DE09h).

        cpu     386
        bits    16
        org     100h

MPX     equ     0deh

start:
        mov     dx, msg_banner
        call    print
        mov     ax, MPX << 8            ; MDVMM installed?
        int     2fh
        cmp     al, 0ffh
        jne     err_mdvmm

        ; slot count
        mov     si, 81h
.sp:    lodsb
        cmp     al, ' '
        je      .sp
        mov     byte [count], 2
        cmp     al, '1'
        jb      .count_done
        cmp     al, '3'
        ja      usage
        sub     al, '0'
        mov     [count], al
.count_done:
        call    find_comspec
        jc      err_comspec

        ; keep only this program: the slots need room for the shell
        mov     bx, (prog_end - $$ + 100h + 15) >> 4
        mov     ah, 4ah
        int     21h
        mov     sp, prog_end - 2

.fork:  mov     ax, (MPX << 8) | 02h
        int     2fh
        jc      err_fork
        test    ax, ax
        jz      slot
        inc     byte [started]
        mov     al, [started]
        cmp     al, [count]
        jb      .fork
        add     byte [msg_n], al
        mov     dx, msg_ok
        call    print
        mov     ax, 4c00h
        int     21h

; ----------------------------------------------------------------------
; a job slot (runs forever in its own session)
slot:
        mov     [saved_sp], sp
.wait:  push    cs
        pop     es
        mov     di, cmdbuf
        mov     ax, (MPX << 8) | 0ah
        int     2fh
        jnc     .job
        sti
        hlt                             ; idle: the monitor runs others
        jmp     .wait
.job:
        ; command tail: empty or " /C <command>"
        mov     si, cmdbuf
        mov     di, tail + 1
        cmp     byte [si], 0
        je      .tail_done
        mov     dword [di], ' /C '
        add     di, 4
.copy:  lodsb
        test    al, al
        jz      .tail_done
        stosb
        cmp     di, tail + 126
        jb      .copy
.tail_done:
        mov     byte [di], 13
        mov     ax, di
        sub     ax, tail + 1
        mov     [tail], al

        mov     ax, 0003h               ; a clean screen for the job
        int     10h

        mov     word [epb], 0           ; inherit the environment
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
        cli                             ; EXEC may not keep SS:SP
        mov     bx, cs
        mov     ss, bx
        mov     sp, [cs:saved_sp]
        sti
        mov     ds, bx
        mov     es, bx
        mov     bx, 0ffffh              ; could not start
        jc      .done
        mov     ah, 4dh
        int     21h
        xor     ah, ah
        mov     bx, ax
.done:  mov     ax, (MPX << 8) | 0bh
        int     2fh
        jmp     .wait

; ----------------------------------------------------------------------

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

print:  mov     ah, 9
        int     21h
        ret

usage:      mov     dx, msg_usage
            jmp     fail
err_mdvmm:  mov     dx, msg_mdvmm
            jmp     fail
err_comspec: mov    dx, msg_comspec
            jmp     fail
err_fork:   mov     dx, msg_fork
fail:       call    print
            mov     ax, 4c01h
            int     21h

msg_banner  db  'SSHSLOT - job slots for SSHD', 13, 10, '$'
msg_ok      db  'Job slots started: '
msg_n       db  '0', 13, 10, '$'
msg_usage   db  'Usage: SSHSLOT [n]   (n = 1..3 slots, default 2)', 13, 10, '$'
msg_mdvmm   db  'MDVMM is not loaded.', 13, 10, '$'
msg_comspec db  'COMSPEC not set.', 13, 10, '$'
msg_fork    db  'Could not start a session (MDVMM sessions set up, a DPMI program running?).', 13, 10, '$'
kw_comspec  db  'COMSPEC='

count       db  2
started     db  0
saved_sp    dw  0
comspec_ptr dd  0
epb         times 14 db 0
tail        times 128 db 0
cmdbuf      times 128 db 0
            times 256 db 0              ; stack
prog_end:
