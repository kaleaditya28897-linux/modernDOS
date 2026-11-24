; MDLOG.COM - show the MDVMM monitor's log (UEFI boots)
;
;   MDLOG        the last 20 lines
;   MDLOG /A     all lines kept (64)
;   MDLOG /N     first ask the network card driver for its state, then
;                the last 20 lines (send a photo of this when the network
;                does not work)
;
; The monitor keeps its log (drivers found, errors) in memory; INT 2Fh
; AX=DE14h copies it out, AX=DE13h makes the network driver add its state.

        cpu     386
        bits    16
        org     100h

start:
        mov     word [lines], 20
        mov     si, 81h
.arg:   lodsb
        cmp     al, 13
        je      .go
        cmp     al, '/'
        jne     .arg
        lodsb
        and     al, 0dfh
        cmp     al, 'A'
        jne     .n
        mov     word [lines], 0
        jmp     .arg
.n:     cmp     al, 'N'
        jne     .arg
        mov     ax, 0de13h              ; driver state into the log
        int     2fh
        jnc     .arg
        mov     dx, msg_nocard
        mov     ah, 9
        int     21h
        jmp     .arg
.go:
        mov     ax, 0de00h              ; MDVMM there?
        int     2fh
        cmp     al, 0ffh
        jne     .nomon
        push    cs
        pop     es
        mov     di, buf
        mov     cx, buf_size
        mov     bx, [lines]
        mov     ax, 0de14h
        int     2fh
        jc      .nolog
        mov     dx, buf                 ; write CX bytes to standard output
        mov     bx, 1
        mov     ah, 40h
        int     21h
        mov     dx, crlf
        mov     ah, 9
        int     21h
        mov     ax, 4c00h
        int     21h
.nomon: mov     dx, msg_nomon
        jmp     .fail
.nolog: mov     dx, msg_nolog
.fail:  mov     ah, 9
        int     21h
        mov     ax, 4c01h
        int     21h

lines       dw  20
crlf        db  13, 10, '$'
msg_nomon   db  'MDVMM is not running.', 13, 10, '$'
msg_nolog   db  'The monitor keeps no log on this boot (DOS boot: see the debug port).', 13, 10, '$'
msg_nocard  db  '(no network card driver is active)', 13, 10, '$'

buf_size    equ 8000
buf:
