#include "interrupt.h"

#include "common.h"
#include "console.h"
#include "global.h"
#include "print.h"
#include "types.h"
/*中断门描述符结构体*/
struct gate_desc {
  uint16_t func_offset_low_word;  // 中断处理程序在目标代码段内的偏移量(低16位)
  uint16_t selector;  // 中断处理程序目标代码段描述符选择子
  uint8_t dcount;     // 该部分未使用
  uint8_t attribute;  // 一些属性标识位(P,DPL,S(0),TYPE(D110))
  uint16_t func_offset_high_word;  // 中断处理程序在目标代码段内的偏移量(高16位)
};

// idt是中断描述符表,本质上就是个中断门描述符数组
static struct gate_desc idt[IDT_DESC_CNT];

// 声明引用定义在kernel.S中的中断处理函数入口数组
extern intr_handler intr_entry_table[IDT_DESC_CNT];
// 用来存放中断的name
char* intr_name[IDT_DESC_CNT];
// 中断处理程序的入口数组（kernel.S中断中的中断处理程序最终会跳到该入口进行执行）
intr_handler idt_table[IDT_DESC_CNT];

/* 初始化可编程中断控制器8259A */
static void pic_init(void) {
  /* 初始化主片 */
  outb(PIC_M_CTRL, 0x11);  // ICW1: 边沿触发,级联8259, 需要ICW4.
  outb(PIC_M_DATA,
       0x20);  // ICW2: 起始中断向量号为0x20,也就是IR[0-7] 为 0x20 ~ 0x27.
  outb(PIC_M_DATA, 0x04);  // ICW3: IR2接从片.
  outb(PIC_M_DATA, 0x01);  // ICW4: 8086模式, 正常EOI

  /* 初始化从片 */
  outb(PIC_S_CTRL, 0x11);  // ICW1: 边沿触发,级联8259, 需要ICW4.
  outb(PIC_S_DATA,
       0x28);  // ICW2: 起始中断向量号为0x28,也就是IR[8-15] 为 0x28 ~ 0x2F.
  outb(PIC_S_DATA, 0x02);  // ICW3: 设置从片连接到主片的IR2引脚
  outb(PIC_S_DATA, 0x01);  // ICW4: 8086模式, 正常EOI

  /* 打开主片上IR0,也就是目前只接受时钟产生的中断 */
  outb(PIC_M_DATA, 0xfe);
  outb(PIC_S_DATA, 0xff);

  console_write("   pic_init done\n");
}

/*创建中断门描述符*/
static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr,
                          intr_handler function) {
  p_gdesc->func_offset_low_word = voidptrTouint32(function) & 0x0000FFFF;
  p_gdesc->selector = SELECTOR_K_CODE;
  p_gdesc->dcount = 0;
  p_gdesc->attribute = attr;
  p_gdesc->func_offset_high_word =
      (voidptrTouint32(function) & 0xFFFF0000) >> 16;
}

/*初始化中断描述符表*/
static void idt_desc_init(void) {
  int i;
  for (i = 0; i < IDT_DESC_CNT; i++) {
    make_idt_desc(&idt[i], IDT_DESC_ATTR_DPL0, intr_entry_table[i]);
  }
  console_write("   idt_desc_init done\n");
}

/* 通用的中断处理函数,一般用在异常出现时的处理 */
static void general_intr_handler(uint8_t vec_nr) {
  if (vec_nr == 0x27 || vec_nr == 0x2f) {
    // IRQ7(用来级联)和IRQ15(保留项)会产生伪中断
    return;  // 伪中断忽略处理
  }
  console_write("int vector : 0x");
  console_write_hex(vec_nr);
  console_write_char('\n');
}

/* 完成一般中断处理函数注册及异常名称注册 */
static void exception_init(void) {
  int i;
  for (i = 0; i < IDT_DESC_CNT; i++) {
    idt_table[i] = general_intr_handler;  // 默认处理函数
    intr_name[i] = "unknow";              // 先统一赋值
  }
  intr_name[0] = "#DE Divide Error";
  intr_name[1] = "#DB Debug Exception";
  intr_name[2] = "NMI Interrupt";
  intr_name[3] = "#BP Breakpoint Exception";
  intr_name[4] = "#OF Overflow Exception";
  intr_name[5] = "#BR BOUND Range Exceeded Exception";
  intr_name[6] = "#UD Invalid Opcode Exception";
  intr_name[7] = "#NM Device Not Available Exception";
  intr_name[8] = "#DF Double Fault Exception";
  intr_name[9] = "Coprocessor Segment Overrun";
  intr_name[10] = "#TS Invalid TSS Exception";
  intr_name[11] = "#NP Segment Not Present";
  intr_name[12] = "#SS Stack Fault Exception";
  intr_name[13] = "#GP General Protection Exception";
  intr_name[14] = "#PF Page-Fault Exception";
  // intr_name[15] 第15项是intel保留项，未使用
  intr_name[16] = "#MF x87 FPU Floating-Point Error";
  intr_name[17] = "#AC Alignment Check Exception";
  intr_name[18] = "#MC Machine-Check Exception";
  intr_name[19] = "#XF SIMD Floating-Point Exception";
}

/*完成有关中断的所有初始化工作*/
void idt_init() {
  console_write("idt_init start\n");
  idt_desc_init();
  exception_init();
  pic_init();  // 初始化 8259A
  // 加载idt到idtr寄存器
  uint64_t idt_operand =
      ((sizeof(idt) - 1) | ((uint64_t)(voidptrTouint32((void*)idt) << 16)));
  asm volatile("lidt %0" : : "m"(idt_operand));

  console_write("idt_init done\n");
}