TI_GDT equ 0
RPL0 equ 0
SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0

[bits 32]

section .data
put_int_buffer dq 0  ; 定义 8 字节缓冲区用于数字到字符的转换
section .text
global put_char
put_char:
    pushad ;将32位的通用寄存器压入栈中
     ;需要保证 gs 中为正确的视频段选择子
     ;为保险起见,每次打印时都为 gs 赋值
    mov ax,SELECTOR_VIDEO
    mov gs, ax

;-----------获取当前光标位置---------
    ;先获取高8位
    mov dx, 0x03d4 ;索引寄存器
    mov al, 0x0e   ;用于提供光标位置的高 8 位
    out dx, al 
    mov dx, 0x3d5  ;通过读写数据端口 0x3d5 来获得或设置光标位置
    in  al, dx     ;得到光标位置的高8位
    mov ah, al
    ;再获取低8位
    mov dx, 0x03d4 
    mov al, 0x0f   
    out dx, al 
    mov dx, 0x3d5  
    in  al, dx     
  
    mov bx, ax     ;将光标存入 bx
    mov ecx, [esp+36] ;获取参数(pushad压入32字节，加上主调函数的返回地址4)
    cmp cl, 0xd ;控制符CR(回车,并将光标移动至开头)
    jz .is_carriage_return
    cmp cl, 0xa ;控制符LF(换行)
    jz .is_line_feed

    cmp cl, 0x8 ;控制符BS(退格，光标左移)
    jz .is_backspace
    jmp .put_other

.is_backspace:
    dec bx  ;减1
    shl bx, 1 ;乘2(bx存放的是字符坐标，*2才是在显存中的位置)
    mov byte [gs:bx],0x20  ;将待删除的字节补为 0 或空格皆可
    inc bx
    mov byte [gs:bx],0x07  ;字符属性(黑屏白字)(一个字符两个字节，低字节存放字符，高字节存放属性)
    shr bx, 1 ;除2(进行恢复，恢复成字符坐标)
    jmp .set_cursor ;刷新光标

.put_other:
    shl bx, 1  ;乘2(bx存放的是字符坐标，*2才是在显存中的位置)

    mov [gs:bx], cl 
    inc bx
    mov byte [gs:bx], 0x07
    shr bx, 1
    inc bx
    cmp bx, 2000   ;此时判断光标是否写到缓存最后
    jl .set_cursor ;没有到达最后则设置新的光标，则跳转


.is_line_feed:          ;是换行符LF(\n)
.is_carriage_return:    ;是回车符号CR(\r)
    xor dx, dx  ;清零(后面用来存放余数)
    mov ax, bx  ;被除数
    mov si, 80
    div si 
    sub bx, dx  ;bx-(bx%80的余数)=该行的行首位置

.is_carriage_return_end:
    add bx, 80  ;移动到下一行的行首位置
    cmp bx, 2000 ;此时判断光标是否写到缓存最后

 .is_line_feed_end:
    jl .set_cursor


.roll_screen: ;滚屏实现，将屏幕的第 1~24 行搬运到第 0~23 行，再将24行用空格填充
    cld
    mov ecx, 960 ;搬运字符次数(2000-80)*2/4=960，一次搬运四个字节，搬运1920个字符，一个字符2个字节
    mov esi, 0xc00b80a0 ;第1行行首
    mov edi, 0xc00b8000 ;第0行行首
    rep movsd   

    ;将最后一行填充为空格
    mov ebx, 3840  ; 最后一行首字符的第一个字节偏移= 1920 * 2
    mov ecx, 80    ;填充次数

.cls:
    mov word [gs:ebx],  0x0720 ;0x0720 是黑底白字的空格键
    add ebx, 2
    loop .cls
    mov bx, 1920    ;光标移动到24行行首

.set_cursor:   ;设置光标
    ;先设置高8位
    mov dx, 0x03d4 ;索引寄存器
    mov al, 0x0e   ;用于提供光标位置的高 8 位
    out dx, al 
    mov dx, 0x3d5  ;通过读写数据端口 0x3d5 来获得或设置光标位置
    mov al, bh
    out dx, al    ;设置光标位置的高8位

    ;再设置低8位
    mov dx, 0x03d4 ;索引寄存器
    mov al, 0x0f   ;用于提供光标位置的低 8 位
    out dx, al 
    mov dx, 0x3d5  ;通过读写数据端口 0x3d5 来获得或设置光标位置
    mov al, bl
    out dx, al     ;设置光标位置的高8位

.put_char_done:
    popad  ;将之前压入栈的通用寄存器出栈
    ret

 ;--------------------------------------------------------------------
 ;put_str 通过 put_char 来打印以 0 字符结尾的字符串
 ;--------------------------------------------------------------------
global put_str
put_str:
    push ebx
    push ecx
    xor ecx,ecx
    mov ebx, [esp + 12] ;从栈中得到待打印的字符串地址
.goon:
    mov cl, [ebx] ;获取字符
    cmp cl, 0     ;如果处理到了字符串尾,跳到结束处返回
    jz .str_over
    push ecx
    call put_char
    add  esp, 4   ;回收栈空间
    inc  ebx      ;指向下一个字符地址
    jmp .goon
.str_over:
    pop ecx
    pop ebx
    ret


global put_int
put_int:
    pushad
    mov ebp, esp 
    mov eax, [ebp+4*9]  ;call的返回地址和pushad的8个四字节
    mov edx, eax
    mov edi, 7 ;偏移量（后面要将低位放在后面，高位放在前面）
    mov ecx, 8 ;循环次数,32 位数字中,十六进制数字的位数是 8 个
    mov ebx, put_int_buffer ;基址

.16based_bits:
    and edx, 0xF ;解析十六进制数字的每一位(只保留低四位)
    cmp edx, 9   ; 数字 0~9 和 a~f 需要分别处理成对应的字符
    jg .is_A2F  ;如果大于则进行跳转
    add edx, '0'
    jmp .store 
.is_A2F: ;大于9的数字转换为a~f
    sub edx, 10
    add edx, 'A'

.store:  ;存储
    mov [ebx+edi], dl 
    dec edi 
    shr eax, 4    
    mov edx, eax  ;存放下一部分数字
    loop .16based_bits

.ready_to_print:
    inc edi   ; 此时 edi 退减为-1(0xffffffff),加 1 使其为 0
.skip_prefix_0: ;跳过前置0
    cmp edi, 8
    je .full0 ;如果相等则跳转

.go_on_skip:
    mov cl, [put_int_buffer+edi]
    inc edi
    cmp cl, '0'
    je .skip_prefix_0  ;继续判断下一位字符是否为字符 0(不是数字 0)
    dec edi ;如果不是0则恢复edi
    jmp .put_each_num

.full0:
    mov cl, '0'
.put_each_num:
    push ecx
    call put_char
    add esp, 4   ;释放参数
    inc edi 
    mov cl, [put_int_buffer+edi] ;获取下一个
    cmp edi, 8
    jl .put_each_num
    popad
    ret



