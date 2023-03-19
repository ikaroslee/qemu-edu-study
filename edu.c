/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "qapi/visitor.h"

#define TYPE_PCI_EDU_DEVICE "edu"
typedef struct EduState EduState;
DECLARE_INSTANCE_CHECKER(EduState, EDU,
                         TYPE_PCI_EDU_DEVICE)

static FILE *fp_edu;
#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000  // DMA Start sddr
#define DMA_SIZE        4096     // 0x1000

struct EduState {
    PCIDevice     pdev;          // PCI device struct
    MemoryRegion  mmio;          // edu mmio memory region 

    QemuThread  thread;          // 线程
    QemuMutex   thr_mutex;       // 线程互斥量
    QemuCond    thr_cond;        // 线程条件变量
    bool        stopping;        // stop edu device

    uint32_t  addr4;             // check edu device is liveness
    uint32_t  fact;              // fact value register
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;             // status register

    uint32_t irq_status;         // irq status register

#define EDU_DMA_RUN             0x1                   // flag: dma is running
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)  // 获取 direction bit field
#define EDU_DMA_FROM_PCI        0
#define EDU_DMA_TO_PCI          1
#define EDU_DMA_IRQ             0x4
    struct dma_state {
        dma_addr_t   src;
        dma_addr_t   dst;
        dma_addr_t   cnt;
        dma_addr_t   cmd;
    } dma;
    QEMUTimer dma_timer;          // DMA定时器
    char      dma_buf[DMA_SIZE];  // DMA buffer
    uint64_t  dma_mask;
};

static bool edu_msi_enabled(EduState *edu)
{
    return msi_enabled(&edu->pdev);
}

static void edu_raise_irq(EduState *edu, uint32_t val)
{
    //fprintf(fp_edu, "%s: %d: val=%d \n",__func__,__LINE__, val);fflush(fp_edu);
    printf("%s: %d:  val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
    edu->irq_status |= val;     // 按位或: irq_status = irq_status | val;
    if (edu->irq_status) {      // 不为0
        if (edu_msi_enabled(edu)) {
            msi_notify(&edu->pdev, 0);
        } else {
            pci_set_irq(&edu->pdev, 1);
        }
    }
}

static void edu_lower_irq(EduState *edu, uint32_t val)
{
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    edu->irq_status &= ~val;

    if (!edu->irq_status && !edu_msi_enabled(edu)) {
        pci_set_irq(&edu->pdev, 0);
    }
}

static bool within(uint64_t addr, uint64_t start, uint64_t end)
{
    return start <= addr && addr < end;
}

static void edu_check_range(uint64_t addr, uint64_t size1, uint64_t start,
                uint64_t size2)
{
    uint64_t end1 = addr + size1;
    uint64_t end2 = start + size2;

    if (within(addr, start, end2) &&
            end1 > addr && within(end1, start, end2)) {
        return;
    }

    hw_error("EDU: DMA range 0x%016"PRIx64"-0x%016"PRIx64
             " out of bounds (0x%016"PRIx64"-0x%016"PRIx64")!",
            addr, end1 - 1, start, end2 - 1);
}

static dma_addr_t edu_clamp_addr(const EduState *edu, dma_addr_t addr)  // clamp: 夹紧，固定 判断该地址是否在DMA mask之内
{
    dma_addr_t res = addr & edu->dma_mask;

    if (addr != res) {
        printf(fp_edu, "EDU: clamping DMA %#.16"PRIx64" to %#.16"PRIx64"!\n", addr, res);fflush(stdout);
    }

    return res;
}
/**
 * --------------------------------------------------------------------------
 * timer_init_ms(&edu->dma_timer, QEMU_CLOCK_VIRTUAL, edu_dma_timer, edu);
 * --------------------------------------------------------------------------
 * timer_init_ms:
 * @ts: the timer to be initialised
 * @type: the clock to associate with the timer
 * @cb: the callback to call when the timer expires
 * @opaque: the opaque pointer to pass to the callback
 *
 * Initialize a timer with millisecond scale on the default timer list
 * associated with the clock.
 * See timer_init_full for details.
 * timer_init_ms：
 * @ts：要初始化的计时器
 * @type：与计时器关联的时钟
 * @cb：定时器到期时调用的回调
 * @opaque：传递给回调的不透明指针
 * 在与时钟关联的默认计时器列表中初始化具有毫秒刻度的计时器。
 有关详细信息，请参阅timer_init_full。
 *  static inline int pci_dma_read(PCIDevice *dev, dma_addr_t addr, void *buf, dma_addr_t len){
 *     return pci_dma_rw(dev, addr, buf, len, DMA_DIRECTION_TO_DEVICE);
 *  }
 * static inline int pci_dma_write(PCIDevice *dev, dma_addr_t addr, const void *buf, dma_addr_t len){
 *    return pci_dma_rw(dev, addr, (void *) buf, len, DMA_DIRECTION_FROM_DEVICE);
 *  }
 */
// 等待 cmd=0bxxxx xxx1 时进行操作
static void edu_dma_timer(void *opaque)   // 定期调用该函数
{
    EduState *edu   = opaque;
    bool raise_irq  = false;
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    if (!(edu->dma.cmd & EDU_DMA_RUN)) { // only cmd=0bxxxxxxx0 时, cmd & 0b00000001 == 0时, 才会return.
        // 换言之，该程序只有等cmd=0bxxxx xxx1时，才会继续执行后面的操作，cmd start dma transfer bit 必须为1
        return;
    }
    // condition: cmd=0b0000 0001
    if (EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_FROM_PCI) {  // DIR: direction：0: from RAM to EDU, 1: from EDU to RAM
        uint64_t dst = edu->dma.dst;
        edu_check_range(dst, edu->dma.cnt, DMA_START, DMA_SIZE);
        dst -= DMA_START;  // DMA：把数据从RAM写到 EDU 的 dma buf中: dst = dst -0x40000 | dst的地址必须大于等于 DMA START即 dst地址:
        pci_dma_read(&edu->pdev, edu_clamp_addr(edu, edu->dma.src), edu->dma_buf + dst, edu->dma.cnt);  
    } else {
        uint64_t src = edu->dma.src;
        edu_check_range(src, edu->dma.cnt, DMA_START, DMA_SIZE);
        src -= DMA_START;  // 1: from EDU to RAM | src的地址必须大于等于 DMA START的地址
        pci_dma_write(&edu->pdev, edu_clamp_addr(edu, edu->dma.dst), edu->dma_buf + src, edu->dma.cnt);
    }

    edu->dma.cmd &= ~EDU_DMA_RUN; // cmd =cmd & (~1) = cmd & 0b11111110 把cmd start transfer bit clear掉
    if (edu->dma.cmd & EDU_DMA_IRQ) {  // Entry: not cmd transfer
        raise_irq = true;  // 拉中断标志
    }

    if (raise_irq) {
        edu_raise_irq(edu, DMA_IRQ); // 拉中断
    }
}

static void dma_rw(EduState *edu, bool write, dma_addr_t *val, dma_addr_t *dma, bool timer)
{
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    if (write && (edu->dma.cmd & EDU_DMA_RUN)) {
        // write=true, edu->dma.cmd ==0b0001  0x01 -- DMA start transfer
        return;  
    }

    if (write) {
        *dma = *val;
    } else {
        *val = *dma;
    }

    if (timer) {
        timer_mod(&edu->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);  
        // 使用timer_mod这个函数注册定时器，设置定时器的到期时间
    }
}

static uint64_t edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    EduState *edu   = opaque;
    uint64_t val    = ~0ULL;
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  addr:0x%x, size:%d, default return val:0x%x \n",__func__,__LINE__, addr, size, val);
    fflush(stdout);
    if (addr < 0x80 && size != 4) {
        printf("%s: %d:  size!=4 return: addr  \n",__func__,__LINE__);fflush(stdout);
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        printf("%s: %d:  addr>=0x80 return: addr  \n",__func__,__LINE__);fflush(stdout);
        return val;
    }

    switch (addr) {
    case 0x00:     // 标识寄存器: 主要版本、次要版本
        val = 0x010000edu;
        printf("%s: %d:  case: 0x00 \n",__func__,__LINE__);fflush(stdout);
        break;
    case 0x04:  // card liveness check,卡活性检测，只是对值进行反转
        val = edu->addr4;
        printf("%s: %d:  case: 0x04 val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
        break;
    case 0x08:  // 阶乘计算：取存储的值，阶乘的值计算后放回此处。只发生在状态寄存器中的阶乘位清除时:0x20 
                //  The stored value is taken and factorial of it is put back here.
	            //  This happens only after factorial bit in the status register (0x20 below) is cleared.
        qemu_mutex_lock(&edu->thr_mutex);  // 加锁: 如果锁被占据，则阻塞当前线程 使用锁机制可以满足线程之间的互斥关系，
        val = edu->fact;
        printf("%s: %d:  case: 0x08 fetch fact val:0x%x  \n",__func__,__LINE__, val);fflush(stdout);
        qemu_mutex_unlock(&edu->thr_mutex); // 解锁
        break;
    case 0x20:  // status register, bitwise OR
                // 0x01 -- computing factorial (RO)
                // 0x80 -- raise interrupt after finishing factorial computation
        val = qatomic_read(&edu->status);
        printf("%s: %d:  case: 0x20 fetch edu status :0x%x  \n",__func__,__LINE__, val);fflush(stdout);
        break;
    case 0x24:  // interrupt status register
                // It contains values which raised the interrupt (see interrupt raise register below).
        val = edu->irq_status;
        printf("%s: %d:  case: 0x24 fetch irq status :0x%x  \n",__func__,__LINE__, val);fflush(stdout);
        break;
    case 0x80:  // DMA source address
        dma_rw(edu, false, &val, &edu->dma.src, false);
        printf("%s: %d:  case: 0x80 \n",__func__,__LINE__);fflush(stdout);
        break;
    case 0x88:  // DMA destination address
        dma_rw(edu, false, &val, &edu->dma.dst, false);
        printf("%s: %d:  case: 0x88 \n",__func__,__LINE__);fflush(stdout);
        break;
    case 0x90:  // DMA transfer count
        dma_rw(edu, false, &val, &edu->dma.cnt, false);
        printf("%s: %d:  case: 0x90 \n",__func__,__LINE__);fflush(stdout);
        break;
    case 0x98:  //  DMA command register, bitwise OR
        dma_rw(edu, false, &val, &edu->dma.cmd, false);  // Read dma.cmd register
        printf("%s: %d:  case: 0x98 \n",__func__,__LINE__);fflush(stdout);
        break;
    default:
        printf("%s: %d:  case: default addr:0x%x ,size:%d \n",__func__,__LINE__, addr, size);fflush(stdout);
    }

    return val;
}

static void edu_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EduState *edu = opaque;
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    if (addr < 0x80 && size != 4) {
        printf("%s: %d:  size!=4 return: addr  \n",__func__,__LINE__);fflush(stdout);
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        printf("%s: %d:  addr>=0x80 return: addr  \n",__func__,__LINE__);fflush(stdout);
        return;
    }

    switch (addr) {
    case 0x04: // card liveness check,卡活性检测，只是对值进行反转
        edu->addr4 = ~val;
        printf("%s: %d:  case: 0x04, write addr4:0x%x \n",__func__,__LINE__, edu->addr4);fflush(stdout);
        break;
    case 0x08: // 阶乘计算：设置阶乘计算原始值
        printf("%s: %d:  case: 0x08 \n",__func__,__LINE__);fflush(stdout);
        if (qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) { // status & 0b0000 0001, 判断 status 是否在计算中
            printf("%s: %d:  case: 0x08 break: status\n",__func__,__LINE__);fflush(stdout);
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because it is only
         * set in this function and it is under the iothread mutex.
         */
        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = val;
        printf("%s: %d:  case: 0x08 write fact:0x%x \n",__func__,__LINE__, val);fflush(stdout);
        qatomic_or(&edu->status, EDU_STATUS_COMPUTING);
        qemu_cond_signal(&edu->thr_cond);  // 唤醒一个等待某个条件变量的线程)
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:  // status register, bitwise OR
                // 0x01 -- computing factorial (RO)
                // 0x80 -- raise interrupt after finishing factorial computation 0x80=0b1000 0000
        printf("%s: %d:  case: 0x20 status register: write val= %x \n",__func__,__LINE__, val);fflush(stdout);
        if (val & EDU_STATUS_IRQFACT) {
            qatomic_or(&edu->status, EDU_STATUS_IRQFACT);   // 设置IRQFACT status bit 为1
            printf("%s: %d:  case: 0x20 qatomic_or: edu->status: %x \n",__func__,__LINE__, edu->status);fflush(stdout);
        } else {
            qatomic_and(&edu->status, ~EDU_STATUS_IRQFACT); // ~EDU_STATUS_IRQFACT=0b0111 1111 清除IRQFACT status
            printf("%s: %d:  case: 0x20 qatomic_and: edu->status: %x \n",__func__,__LINE__, edu->status);fflush(stdout);
        }
        break;
    case 0x60:  // interrupt raise register
	            // Raise an interrupt. The value will be put to the interrupt status register (using bitwise OR).
        printf("%s: %d:  case: 0x60 interrupt raise register val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
        edu_raise_irq(edu, val);
        break;
    case 0x64:  // interrupt acknowledge register  中断确认寄存器
                // Clear an interrupt. The value will be cleared from the interrupt
	            // status register. This needs to be done from the ISR to stop generating interrupts.
        printf("%s: %d:  case: 0x64 interrupt lower register val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
        edu_lower_irq(edu, val);
        break;
    case 0x80:  // DMA source address
                // Where to perform the DMA from. 从何处执行DMA
        printf("%s: %d:  case: 0x80 DMA write source: val:0x%x, edu->dma.src=0x%x \n",__func__,__LINE__, val, edu->dma.src);fflush(stdout);
        dma_rw(edu, true, &val, &edu->dma.src, false);
        break;
    case 0x88:  //  DMA destination address
                // Where to perform the DMA to.
        printf("%s: %d:  case: 0x88 DMA write destination: val:0x%x, edu->dma.dst=0x%x \n",__func__,__LINE__, val, edu->dma.dst);fflush(stdout);
        dma_rw(edu, true, &val, &edu->dma.dst, false);
        break;
    case 0x90:  // DMA transfer count
                // The size of the area to perform the DMA on.
        printf("%s: %d:  case: 0x90 DMA transfer count: val:0x%x, edu->dma.cnt=0x%x \n",__func__,__LINE__, val, edu->dma.cnt);fflush(stdout);
        dma_rw(edu, true, &val, &edu->dma.cnt, false);
        break;
    case 0x98:  //  DMA command register, bitwise OR
                //  0x01 -- start transfer                                     0b0001
                //  0x02 -- direction (0: from RAM to EDU, 1: from EDU to RAM) 0b0010
                //  0x04 -- raise interrupt 0x100 after finishing the DMA      0b0100
        printf("%s: %d:  case: 0x98 DMA cmd reg: val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
        if (!(val & EDU_DMA_RUN)) {  // only val=0x1, inner condition is true, not entey.
            printf("%s: %d:  case: 0x98 : val:0x%x \n",__func__,__LINE__, val);fflush(stdout);
            break;
        }
        // val=0b0000 0001 =0x1: DMA start transfer | dir: from RAM to edu 
        // val=0b0000 0011 =0x3: | dir: from EDU to RAM
        // val=0b0000 0101 =0x5: | raise interrupt
        printf("%s: %d:  case: 0x98 DMA cmd reg: before write val:0x%x, edu->dma.cmd=0x%x \n",__func__,__LINE__, val, edu->dma.cmd);fflush(stdout);
        dma_rw(edu, true, &val, &edu->dma.cmd, true);
        printf("%s: %d:  case: 0x98 DMA cmd reg: after write val:0x%x, edu->dma.cmd=0x%x \n",__func__,__LINE__, val, edu->dma.cmd);fflush(stdout);
        break;
    default:
        printf("%s: %d:  case: default addr:0x%x , val:0x%x , size:%d \n",__func__,__LINE__, addr, size);fflush(stdout);
    }
}

static const MemoryRegionOps edu_mmio_ops = {
    .read       = edu_mmio_read,
    .write      = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};

/*
 * We purposely use a thread, so that users are forced to wait for the status
 * register.
 */
static void *edu_fact_thread(void *opaque)  // 起一个计算阶乘的线程函数
{
    EduState *edu = opaque;
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("---------------------------------------- \n");fflush(stdout);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    printf("---------------------------------------- \n");fflush(stdout);
    while (1) {
        uint32_t val, ret = 1;
        //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
        printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
        qemu_mutex_lock(&edu->thr_mutex);
        while ((qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) == 0 && !edu->stopping) {
            // True cond: edu->status=0bxxxx xxx0, not on computing,  edu->stopping=0
            qemu_cond_wait(&edu->thr_cond, &edu->thr_mutex); // 函数需要在获得锁之后进行调用，否则可能会出现死锁, 
                                                             // 运行前加锁、运行后释放锁
        }
        // only: edu->status=0bxxxx xxx1 == EDU_STATUS_COMPUTING, edu->stopping==0
        if (edu->stopping) {
            qemu_mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);

        while (val > 0) {  // 计算所给数值的阶乘， 返回值为ret
            ret *= val--;
        }

        /*
         * We should sleep for a random period here, so that students are
         * forced to check the status properly.
         */

        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = ret;
        qemu_mutex_unlock(&edu->thr_mutex);
        qatomic_and(&edu->status, ~EDU_STATUS_COMPUTING); // clear edu->status computing status bit

        if (qatomic_read(&edu->status) & EDU_STATUS_IRQFACT) {
            qemu_mutex_lock_iothread();
            edu_raise_irq(edu, FACT_IRQ);
            qemu_mutex_unlock_iothread();
        }
    }

    return NULL;
}

static void pci_edu_realize(PCIDevice *pdev, Error **errp)
{
    EduState *edu       = EDU(pdev);
    uint8_t *pci_conf   = pdev->config;
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&edu->dma_timer, QEMU_CLOCK_VIRTUAL, edu_dma_timer, edu);

    qemu_mutex_init(&edu->thr_mutex);
    qemu_cond_init(&edu->thr_cond);
    // qemu_thread_create这个函数会调用pthread_create创建一个线程
    // void qemu_thread_create(QemuThread *thread, const char *name, void *(*start_routine)(void *), 
    //                         void *arg, int mode);
    qemu_thread_create(&edu->thread, "edu", edu_fact_thread, edu, QEMU_THREAD_JOINABLE);

    memory_region_init_io(&edu->mmio, OBJECT(edu), &edu_mmio_ops, edu, "edu-mmio", 1 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &edu->mmio);
}

static void pci_edu_uninit(PCIDevice *pdev)
{
    EduState *edu       = EDU(pdev);
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    qemu_mutex_lock(&edu->thr_mutex);
    edu->stopping       = true;
    qemu_mutex_unlock(&edu->thr_mutex);
    qemu_cond_signal(&edu->thr_cond);
    qemu_thread_join(&edu->thread);

    qemu_cond_destroy(&edu->thr_cond);
    qemu_mutex_destroy(&edu->thr_mutex);

    timer_del(&edu->dma_timer);
    msi_uninit(pdev);
    fclose(fp_edu);
}

static void edu_instance_init(Object *obj)
{
    EduState *edu = EDU(obj);
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    edu->dma_mask = (1UL << 28) - 1;
    object_property_add_uint64_ptr(obj, "dma_mask",
                                   &edu->dma_mask, OBJ_PROP_FLAG_READWRITE);
}
\
static void edu_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc     = DEVICE_CLASS(class);
    PCIDeviceClass *k   = PCI_DEVICE_CLASS(class);
    fp_edu = fopen("eduRun.log","w+");
    //fprintf(fp_edu, "%s: %d: \n", __func__,__LINE__);fflush(fp_edu);
    printf("%s: %d:  \n",__func__,__LINE__);fflush(stdout);
    k->realize          = pci_edu_realize;
    k->exit             = pci_edu_uninit;
    k->vendor_id        = PCI_VENDOR_ID_QEMU;
    k->device_id        = 0x11e8;
    k->revision         = 0x10;
    k->class_id         = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_edu_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };
    static const TypeInfo edu_info = {
        .name               = TYPE_PCI_EDU_DEVICE,
        .parent             = TYPE_PCI_DEVICE,
        .instance_size      = sizeof(EduState),
        .instance_init      = edu_instance_init,
        .class_init         = edu_class_init,
        .interfaces         = interfaces,
    };

    type_register_static(&edu_info);
}
type_init(pci_edu_register_types)
