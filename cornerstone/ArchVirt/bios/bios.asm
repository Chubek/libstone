; ArchVirt PC BIOS B1. Original ROM, NASM flat binary, 80386+ real mode.
; Polled console/disk; one CPU; no hardware interrupt sources.
bits 16
org 0
%define CFG_RAM 0xff00
%define CFG_SECTORS 0xff04
%define MAP 0xff10

post:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    xor di, di
    mov cx, 256
.vectors:
    mov ax, unsupported
    stosw
    mov ax, 0xf000
    stosw
    loop .vectors
    xor ax, ax
    mov cx, 128
    rep stosw                       ; BDA 400..4ff
%macro vector 2
    mov word [%1*4], %2
%endmacro
    vector 0x10, video
    vector 0x11, equipment
    vector 0x12, base_memory
    vector 0x13, disk
    vector 0x15, system
    vector 0x16, keyboard
    vector 0x19, bootstrap
    mov word [0x400], 0x3f8
    mov word [0x40e], 0x9f00
    mov word [0x410], 0x21          ; color display and serial port
    mov word [0x413], 636
    mov byte [0x449], 3
    mov word [0x44a], 80
    mov byte [0x475], 1
    mov al, 0xff
    out 0x21, al
    out 0xa1, al
    mov dx, 0x3f6
    mov al, 2                     ; ATA nIEN
    out dx, al
    mov ax, 3
    int 0x10
    mov si, banner
    call puts
bootstrap:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov bx, 0x7c00
    mov ax, 0x0201
    mov cx, 1
    mov dx, 0x0080
    int 0x13
    jc .failed
    cmp word [0x7dfe], 0xaa55
    jne .failed
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov dl, 0x80
    cld
    sti
    jmp 0:0x7c00
.failed:
    mov si, no_boot
    call puts
    cli
    hlt
    jmp $

puts:
    mov al, [cs:si]
    inc si
    test al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp puts
.done: ret
banner: db 'ArchVirt BIOS B1',13,10,0
no_boot: db 'BIOS: no bootable disk',13,10,0

unsupported:
    push bp
    mov bp, sp
    or word [ss:bp+6], 1
    mov ah, 0x86
    pop bp
    iret
equipment:
    mov ax, 0x21
    iret
base_memory:
    mov ax, 636
    iret

; PUSHAD saves EAX at BP-4, ECX -8, EDX -12, EBX -16.
%macro frame 0
    push bp
    mov bp, sp
    pushad
    push ds
    push es
    cld
%endmacro
%macro unframe 0
    pop es
    pop ds
    popad
    pop bp
    iret
%endmacro

video:
    frame
    xor bx, bx
    mov ds, bx
    cmp ah, 0x0e
    je .tty
    cmp ah, 0
    je .mode
    cmp ah, 0x0f
    je .query
    cmp ah, 2
    je .set_cursor
    cmp ah, 3
    je .get_cursor
    jmp .done
.query:
    mov word [ss:bp-4], 0x5003
    mov byte [ss:bp-15], 0
    jmp .done
.set_cursor:
    cmp dh, 25
    jae .done
    cmp dl, 80
    jae .done
    mov [0x450], dx
    jmp .done
.get_cursor:
    mov dx, [0x450]
    mov [ss:bp-12], dx
    mov word [ss:bp-8], 0x0607
    jmp .done
.mode:
    cmp al, 3
    jne .done
    mov ax, 0xb800
    mov es, ax
    xor di, di
    mov ax, 0x0720
    mov cx, 2000
    rep stosw
    mov word [0x450], 0
    jmp .done
.tty:
    mov dx, 0x3f8
    out dx, al
    mov bx, [0x450]                ; BH row, BL column
    cmp al, 13
    je .cr
    cmp al, 10
    je .lf
    cmp al, 8
    je .back
    mov ah, al
    movzx di, bh
    imul di, di, 80
    movzx dx, bl
    add di, dx
    shl di, 1
    mov dx, 0xb800
    mov es, dx
    mov al, ah
    mov ah, 7
    mov [es:di], ax
    inc bl
    cmp bl, 80
    jb .cursor
    xor bl, bl
.lf:
    inc bh
    cmp bh, 25
    jb .cursor
    push ds
    mov ax, 0xb800
    mov ds, ax
    mov es, ax
    mov si, 160
    xor di, di
    mov cx, 1920
    rep movsw
    mov ax, 0x0720
    mov cx, 80
    rep stosw
    pop ds
    mov bh, 24
    jmp .cursor
.cr: xor bl, bl
    jmp .cursor
.back:
    test bl, bl
    jz .cursor
    dec bl
.cursor: mov [0x450], bx
.done:
    unframe

keyboard:
    push bp
    mov bp, sp
    push dx
    cmp ah, 0
    je .read
    cmp ah, 1
    jne .bad
    mov dx, 0x3fd
    in al, dx
    test al, 1
    jz .empty
    ; Peek is buffered in the BDA; subsequent checks do not consume it.
    push ds
    xor ax, ax
    mov ds, ax
    cmp byte [0x496], 0
    jne .peeked
    mov dx, 0x3f8
    in al, dx
    mov [0x496], al
.peeked:
    mov al, [0x496]
    pop ds
    xor ah, ah
    and word [ss:bp+6], ~0x40
    jmp .done
.empty:
    push ds
    xor ax, ax
    mov ds, ax
    mov al, [0x496]
    pop ds
    test al, al
    jnz .available
    or word [ss:bp+6], 0x40
    jmp .done
.available:
    and word [ss:bp+6], ~0x40
    jmp .done
.read:
    push ds
    xor ax, ax
    mov ds, ax
    mov al, [0x496]
    mov byte [0x496], 0
    pop ds
    test al, al
    jnz .done
.poll:
    mov dx, 0x3fd
    in al, dx
    test al, 1
    jz .poll
    mov dx, 0x3f8
    in al, dx
    xor ah, ah
    jmp .done
.bad: mov ah, 0x86
.done:
    pop dx
    pop bp
    iret

system:
    frame
    cmp eax, 0xe820
    jne .legacy
    cmp edx, 0x534d4150
    jne .bad
    cmp ecx, 20
    jb .bad
    cmp ebx, 4
    jae .bad
    cmp di, 0xffe8
    ja .bad
    mov ax, cs
    mov ds, ax
    imul si, bx, 24
    add si, MAP
    mov cx, 20
    cmp dword [ss:bp-8], 24
    jb .copy
    mov cx, 24
.copy:
    movzx eax, cx
    mov [ss:bp-8], eax
    rep movsb
    inc ebx
    cmp ebx, 4
    jb .more
    xor ebx, ebx
.more:
    mov [ss:bp-16], ebx
    mov dword [ss:bp-4], 0x534d4150
    jmp .ok
.legacy:
    cmp ah, 0x88
    jne .a20
    mov eax, [cs:CFG_RAM]
    sub eax, 0x100000
    shr eax, 10
    cmp eax, 65535
    jbe .size
    mov eax, 65535
.size:
    mov [ss:bp-4], ax
    jmp .ok
.a20:
    cmp ax, 0x2401
    jne .bad
    and dword [ss:bp-4], 0xffff00ff
.ok:
    and word [ss:bp+6], ~1
    unframe
.bad:
    mov byte [ss:bp-3], 0x86
    or word [ss:bp+6], 1
    unframe

disk:
    frame
    cmp dl, 0x80
    jne .bad
    test ah, ah
    jz .ok
    cmp ah, 8
    je .geometry
    cmp ah, 0x41
    je .extensions
    cmp ah, 0x48
    je .parameters
    cmp ah, 2
    je .chs
    cmp ah, 3
    je .chs
    cmp ah, 0x42
    je .edd
    cmp ah, 0x44
    je .edd
    cmp ah, 0x47
    je .edd
    cmp ah, 0x43
    jne .bad
    cmp al, 1
    ja .bad
.edd:
    cmp si, 0xfff0
    ja .bad
    cmp word [ds:si], 0x0010
    jne .bad
    cmp dword [ds:si+12], 0
    jne .bad
    mov cx, [ds:si+2]
    mov di, [ds:si+4]
    mov es, [ds:si+6]
    mov eax, [ds:si+8]
    cmp byte [ss:bp-3], 0x47
    jne .transfer
    mov cx, 1                     ; seek uses the starting LBA only
    jmp .transfer
.chs:
    movzx edi, cl
    and edi, 63
    jz .bad
    cmp dh, 16
    jae .bad
    dec edi
    movzx eax, ch
    movzx ebx, cl
    and ebx, 0xc0
    shl ebx, 2
    or eax, ebx
    imul eax, eax, 16
    movzx ebx, dh
    add eax, ebx
    imul eax, eax, 63
    add eax, edi
    mov di, [ss:bp-16]             ; original BX
    movzx cx, byte [ss:bp-4]       ; original AL
.transfer:
    test cx, cx
    jz .bad
    cmp cx, 127
    ja .bad
    movzx edx, cx
    add edx, eax
    jc .bad
    cmp edx, [cs:CFG_SECTORS]
    ja .bad
    cmp byte [ss:bp-3], 0x44
    je .ok                        ; valid sectors in the in-memory medium
    cmp byte [ss:bp-3], 0x47
    je .ok
    movzx ebx, cx
    shl ebx, 9
    movzx edx, di
    add edx, ebx
    cmp edx, 0x10000
    ja .bad
    mov bx, es
    movzx ebx, bx
    shl ebx, 4
    add edx, ebx
    cmp edx, 0x9f000
    ja .bad
    movzx edx, di
    add edx, ebx
    cmp edx, 0x500
    jb .bad
    mov si, 0x20
    test byte [ss:bp-3], 1          ; AH=03 or 43 writes
    jz .next
    mov si, 0x30
.next:
    push eax
    push cx
    mov ebx, eax
    mov dx, 0x1f2
    mov al, 1
    out dx, al
    inc dx
    mov eax, ebx
    out dx, al
    inc dx
    shr eax, 8
    out dx, al
    inc dx
    shr eax, 8
    out dx, al
    inc dx
    shr eax, 8
    and al, 15
    or al, 0xe0
    out dx, al
    inc dx
    mov ax, si
    out dx, al
    mov cx, 0xffff
.poll:
    in al, dx
    test al, 1
    jnz .io_fail
    test al, 0x80
    jnz .again
    test al, 8
    jnz .data
.again:
    loop .poll
.io_fail:
    pop cx
    pop eax
    jmp .bad
.data:
    mov dx, 0x1f0
    mov cx, 256
    cmp si, 0x30
    je .write
    rep insw
    jmp .sector_done
.write:
    mov ax, [es:di]
    out dx, ax
    add di, 2
    loop .write
.sector_done:
    pop cx
    pop eax
    inc eax
    loop .next
    jmp .ok
.geometry:
    ; Logical translation: 16 heads, 63 sectors, whole cylinders only.
    mov eax, [cs:CFG_SECTORS]
    xor edx, edx
    mov ebx, 1008
    div ebx
    test eax, eax
    jz .bad
    dec eax
    cmp eax, 1023
    jbe .cylinder
    mov eax, 1023
.cylinder:
    mov ch, al
    shr eax, 2
    and al, 0xc0
    or al, 63
    mov cl, al
    mov [ss:bp-8], cx
    mov word [ss:bp-12], 0x0f01
    jmp .ok
.extensions:
    cmp bx, 0x55aa
    jne .bad
    mov word [ss:bp-16], 0xaa55
    mov word [ss:bp-8], 1
    mov byte [ss:bp-3], 0x20
    and word [ss:bp+6], ~1
    unframe
.parameters:
    cmp si, 0xffe2
    ja .bad
    cmp word [ds:si], 0x1a
    jb .bad
    mov word [ds:si], 0x1a
    mov word [ds:si+2], 0
    mov dword [ds:si+4], 0
    mov dword [ds:si+8], 16
    mov dword [ds:si+12], 63
    mov eax, [cs:CFG_SECTORS]
    mov [ds:si+16], eax
    mov dword [ds:si+20], 0
    mov word [ds:si+24], 512
.ok:
    mov byte [ss:bp-3], 0
    and word [ss:bp+6], ~1
    unframe
.bad:
    mov byte [ss:bp-3], 1
    or word [ss:bp+6], 1
    unframe

times CFG_RAM-($-$$) db 0
dd 64*1024*1024, 0                ; runtime patches RAM and disk size
times MAP-($-$$) db 0
dq 0, 0x9f000
dd 1, 1
dq 0x9f000, 0x61000
dd 2, 1
dq 0x100000, 63*1024*1024          ; runtime patches usable high RAM length
dd 1, 1
dq 0xffff0000, 0x10000
dd 2, 1
times 0xfff0-($-$$) db 0
jmp 0xf000:post
db '11/15/26', 0, 0xfc
times 65536-($-$$) db 0
