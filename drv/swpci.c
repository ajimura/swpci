/* by Shuhei Ajimura */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
//#include <linux/jiffies.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/blkdev.h>
#include <asm/io.h>
#include "swpci.h"

#define VERB 1
#define DevsNum 1
#define TIMEOUT 2*HZ

#define SWP_BAR_NUM (6)
#define SWP_BAR_MEM (0)
#define SWP_BAR_CTL (2)
#define SWP_BAR_SPW (4)

/* Specifies those BARs to be mapped and the length of each mapping. */
//static const unsigned long bar_min_len[SWP_BAR_NUM] = {62144,0,256,0,0,0};
//static const unsigned long bar_min_len[SWP_BAR_NUM] = {4194304,0,32768,0,0,0};
static const unsigned long bar_min_len[SWP_BAR_NUM] = {2097152,0,32768,0,0,0};

/* Board specific book keeping data */
struct swpci_dev {
  struct pci_dev *pci_dev;
  void * __iomem bar[SWP_BAR_NUM];/*kernel virt. addr of the mapped BAR*/
  //  void * __iomem csr_ptr;
  //  void * __iomem data_ptr;
  spinlock_t lock;
  int msi_enabled;
  u8 revision;
  int in_use;
  int got_regions;
  int irq_line;
  int irq_count;
  wait_queue_head_t wq;
  int wqcndtn;
  dev_t cdevno;
  struct cdev cdev;
  struct swpci_devinfo *devinfo;
  u8 *dmaRbuf_virt;
  dma_addr_t dmaRbuf_phys;
  u8 *dmaWbuf_virt;
  dma_addr_t dmaWbuf_phys;
  u32 a2p_mask;
  u32 a2p_seg;
};
static int wqcond=0;

static const struct pci_device_id ids[] = {
  { PCI_DEVICE(0x1172, 0xE001), },{ 0, }
};
MODULE_DEVICE_TABLE(pci, ids);

unsigned long jiffies_open;
int refcount;

/* prototype */
static int swdev_init(struct swpci_dev *);
static void swdev_exit(struct swpci_dev *);

/* sw_isr() - Interrupt handler */
static irqreturn_t sw_isr(int irq, void *dev_id)
{
  struct swpci_dev *swp = (struct swpci_dev *)dev_id;
#if VERB
  printk(KERN_DEBUG "Get Interrupt at %d\n",irq);
#endif
  if (!swp) return IRQ_NONE;
  wqcond=1;
  wake_up(&swp->wq);
  swp->irq_count++;
  return IRQ_HANDLED;
}

/* scan, unmap, map bars */
static int scan_bars(struct swpci_dev *swp, struct pci_dev *dev)
{
  int i;
  for (i = 0; i < SWP_BAR_NUM; i++) {
    //    printk(KERN_DEBUG "Scanning#%d\n",i);
    unsigned long bar_start = pci_resource_start(dev, i);
    if (bar_start) {
      unsigned long bar_end = pci_resource_end(dev, i);
      unsigned long bar_flags = pci_resource_flags(dev, i);
      unsigned long bar_len = pci_resource_len(dev, i);
      printk(KERN_DEBUG "BAR%d 0x%08lx-0x%08lx flags 0x%08lx  len 0x%08lx\n",
	     i, bar_start, bar_end, bar_flags, bar_len);
    }
  }
  return 0;
}

static void unmap_bars(struct swpci_dev *swp, struct pci_dev *dev)
{
  int i;
  for (i = 0; i < SWP_BAR_NUM; i++)
    if (swp->bar[i]) { iounmap(swp->bar[i]); swp->bar[i] = NULL;}
}

static int map_bars(struct swpci_dev *swp, struct pci_dev *dev)
{
  int rc;
  int i;
  /* iterate through all the BARs */
  for (i = 0; i < SWP_BAR_NUM; i++) {
    unsigned long bar_start  = pci_resource_start(dev, i);
    unsigned long bar_end    = pci_resource_end(dev, i);
    unsigned long bar_length = bar_end - bar_start + 1;
    swp->bar[i] = NULL;
    /* do not map, and skip, BARs with length 0 */
    if (!bar_min_len[i]) continue;
    /* do not map BARs with address 0 */
    if (!bar_start || !bar_end) {
      printk(KERN_DEBUG "BAR #%d is not present?!\n", i);
      rc = -1;
      goto fail;
    }
    bar_length = bar_end - bar_start + 1;
    /* BAR length is less than driver requires? */
    if (bar_length < bar_min_len[i]) {
      printk(KERN_DEBUG "BAR #%d length = %lu bytes but driver "
    	     "requires at least %lu bytes\n", i, bar_length, bar_min_len[i]);
      rc = -1;
      goto fail;
    }
    swp->bar[i] = ioremap_nocache(bar_start, bar_min_len[i]);
    //    swp->bar[i] = ioremap_nocache(bar_start, bar_length);
    if (!swp->bar[i]) {
      printk(KERN_DEBUG "Could not map BAR #%d.\n", i);
      rc = -1;
      goto fail;
    }
    printk(KERN_DEBUG "BAR[%d] mapped at 0x%p with length %lu(/%lu).\n", i,
	   swp->bar[i], bar_min_len[i], bar_length);
  }
  rc = 0;
  goto success;
 fail:
  unmap_bars(swp, dev);
 success:
  return rc;
}

unsigned char rmap_calc_crc(void *,unsigned int );
int rmap_create_buffer(unsigned char, unsigned char, unsigned char, unsigned char, unsigned int, unsigned int, unsigned int, unsigned int);
unsigned char tx_buffer[1024];
unsigned char RM_CRCTbl [] = {
  0x00,0x91,0xe3,0x72,0x07,0x96,0xe4,0x75,  0x0e,0x9f,0xed,0x7c,0x09,0x98,0xea,0x7b,
  0x1c,0x8d,0xff,0x6e,0x1b,0x8a,0xf8,0x69,  0x12,0x83,0xf1,0x60,0x15,0x84,0xf6,0x67,
  0x38,0xa9,0xdb,0x4a,0x3f,0xae,0xdc,0x4d,  0x36,0xa7,0xd5,0x44,0x31,0xa0,0xd2,0x43,
  0x24,0xb5,0xc7,0x56,0x23,0xb2,0xc0,0x51,  0x2a,0xbb,0xc9,0x58,0x2d,0xbc,0xce,0x5f,
  0x70,0xe1,0x93,0x02,0x77,0xe6,0x94,0x05,  0x7e,0xef,0x9d,0x0c,0x79,0xe8,0x9a,0x0b,
  0x6c,0xfd,0x8f,0x1e,0x6b,0xfa,0x88,0x19,  0x62,0xf3,0x81,0x10,0x65,0xf4,0x86,0x17,
  0x48,0xd9,0xab,0x3a,0x4f,0xde,0xac,0x3d,  0x46,0xd7,0xa5,0x34,0x41,0xd0,0xa2,0x33,
  0x54,0xc5,0xb7,0x26,0x53,0xc2,0xb0,0x21,  0x5a,0xcb,0xb9,0x28,0x5d,0xcc,0xbe,0x2f,
  0xe0,0x71,0x03,0x92,0xe7,0x76,0x04,0x95,  0xee,0x7f,0x0d,0x9c,0xe9,0x78,0x0a,0x9b,
  0xfc,0x6d,0x1f,0x8e,0xfb,0x6a,0x18,0x89,  0xf2,0x63,0x11,0x80,0xf5,0x64,0x16,0x87,
  0xd8,0x49,0x3b,0xaa,0xdf,0x4e,0x3c,0xad,  0xd6,0x47,0x35,0xa4,0xd1,0x40,0x32,0xa3,
  0xc4,0x55,0x27,0xb6,0xc3,0x52,0x20,0xb1,  0xca,0x5b,0x29,0xb8,0xcd,0x5c,0x2e,0xbf,
  0x90,0x01,0x73,0xe2,0x97,0x06,0x74,0xe5,  0x9e,0x0f,0x7d,0xec,0x99,0x08,0x7a,0xeb,
  0x8c,0x1d,0x6f,0xfe,0x8b,0x1a,0x68,0xf9,  0x82,0x13,0x61,0xf0,0x85,0x14,0x66,0xf7,
  0xa8,0x39,0x4b,0xda,0xaf,0x3e,0x4c,0xdd,  0xa6,0x37,0x45,0xd4,0xa1,0x30,0x42,0xd3,
  0xb4,0x25,0x57,0xc6,0xb3,0x22,0x50,0xc1,  0xba,0x2b,0x59,0xc8,0xbd,0x2c,0x5e,0xcf
};

/* SW_OPEN */
static int swpci_open(struct inode *inode, struct file *file)
{
  struct swpci_dev *swp;
  int major,minor;

  swp = container_of(inode->i_cdev, struct swpci_dev, cdev);
  file->private_data = swp;

  //  spin_lock(&swp->lock);

  major=imajor(inode);
  minor=iminor(inode);

  if (refcount!=0){
    printk(KERN_DEBUG DRV_NAME "%d:%d is already opened\n",major,minor);
    //    spin_unlock(&swp->lock);
    return -EBUSY;
  }else{
    refcount=1;
  }

  printk(KERN_DEBUG DRV_NAME "_open(): %d:%d %lu/%d\n",major,minor,jiffies,HZ);
  jiffies_open=jiffies;

  swp->dmaRbuf_virt=
    (u8 *)pci_alloc_consistent(swp->pci_dev,PCIE_BUFSIZ,&swp->dmaRbuf_phys);
  if (!swp->dmaRbuf_virt){
    printk(KERN_DEBUG "Could not allocate DMA R buffer.\n");
    return -EFAULT;
  }
  swp->dmaWbuf_virt=
    (u8 *)pci_alloc_consistent(swp->pci_dev,PCIE_BUFSIZ,&swp->dmaWbuf_phys);
  if (!swp->dmaWbuf_virt){
    printk(KERN_DEBUG "Could not allocate DMA W buffer.\n");
    return -EFAULT;
  }

#if VERB
  printk(KERN_DEBUG "DMA R buffer (virt. addr = 0x%p, bus addr = 0x%016llx).\n",
	 swp->dmaRbuf_virt, (u64)swp->dmaRbuf_phys);
  printk(KERN_DEBUG "DMA W buffer (virt. addr = 0x%p, bus addr = 0x%016llx).\n",
	 swp->dmaWbuf_virt, (u64)swp->dmaWbuf_phys);
#endif

  /* address tanslation table */
  iowrite32(0xfffffffc,swp->bar[2]+AVMM_AD0L);
  //  write_bar(swp->bar[2],AVMM_AD0L,0xfffffffc);
  //  *((u32 *)swp->bar[2]+0x1000/4)=0xfffffffc;
  wmb();
  //  swp->a2p_mask=*((u32 *)swp->bar[2]+0x1000/4);
  swp->a2p_mask=ioread32(swp->bar[2]+AVMM_AD0L);
  swp->a2p_seg=(~swp->a2p_mask)+1;
  if(swp->a2p_mask == 0 ) {
    printk(" !!error!! a2p_mask = 0x%x a2p_seg = 0x%x\n", swp->a2p_mask, swp->a2p_seg );
    goto err;
  }
  iowrite32(swp->dmaRbuf_phys & swp->a2p_mask, swp->bar[2]+AVMM_AD0L);
  iowrite32(0,                                 swp->bar[2]+AVMM_AD0H);
  iowrite32(swp->dmaWbuf_phys & swp->a2p_mask, swp->bar[2]+AVMM_AD1L);
  iowrite32(0,                                 swp->bar[2]+AVMM_AD1H);
  //  write_bar(swp->bar[2],AVMM_AD0L,swp->dmaRbuf_phys & swp->a2p_mask);
  //  write_bar(swp->bar[2],AVMM_AD0H,0);
  //  write_bar(swp->bar[2],AVMM_AD1L,swp->dmaWbuf_phys & swp->a2p_mask);
  //  write_bar(swp->bar[2],AVMM_AD1H,0);
  //  *((u32 *)swp->bar[2]+0x1000/4)=swp->dmaRbuf_phys & swp->a2p_mask;
  //  *((u32 *)swp->bar[2]+0x1004/4)=0;
  //  *((u32 *)swp->bar[2]+0x1008/4)=swp->dmaWbuf_phys & swp->a2p_mask;
  //  *((u32 *)swp->bar[2]+0x100c/4)=0;
  wmb();
#if VERB
  printk("  a2p_mask = 0x%08x  a2p_seg = 0x%08x\n", swp->a2p_mask, swp->a2p_seg );
#endif

  //  spin_unlock(&swp->lock);

  return 0;

 err:
  pci_free_consistent(swp->pci_dev,PCIE_BUFSIZ,swp->dmaRbuf_virt,swp->dmaRbuf_phys);
  pci_free_consistent(swp->pci_dev,PCIE_BUFSIZ,swp->dmaWbuf_virt,swp->dmaWbuf_phys);
  return -1;
}

/* SW_CLOSE */
static int swpci_close(struct inode *inode, struct file *file)
{
  struct swpci_dev *swp = file->private_data;

  //  spin_lock(&swp->lock);

  printk(KERN_DEBUG DRV_NAME "_close(): %lu %lu/%d\n",
	 jiffies,jiffies-jiffies_open,HZ);

  /* release DMA buffer */
  pci_free_consistent(swp->pci_dev,PCIE_BUFSIZ,swp->dmaRbuf_virt,swp->dmaRbuf_phys);
  pci_free_consistent(swp->pci_dev,PCIE_BUFSIZ,swp->dmaWbuf_virt,swp->dmaWbuf_phys);

  refcount=0;

  //  spin_unlock(&swp->lock);

  return 0;
}

/* sw_ioctl */
static long swpci_ioctl(
  struct file *file,
  unsigned int cmd,
  unsigned long arg)
{
  struct swpci_dev *swp = file->private_data;
  void * __iomem bar0top=(unsigned int *)swp->bar[0];
  void * __iomem bar2top=(unsigned int *)swp->bar[2];

  struct swio_mem cmd_mem;
  unsigned int address;
  unsigned int addr_rxcsr;
  unsigned int addr_txcsr;
  unsigned int phyaddr;
  unsigned int port;
  int real_len;
  int get_size;
  int put_size;
  int max_size;
  unsigned char buftop[12];
  unsigned int data;
  long remain_time;
  unsigned int ret_tid;

  int retval = 0;

  if (!access_ok(VERIFY_READ, (void __user *)arg,_IOC_SIZE(cmd))){
    retval=-EFAULT; goto done; }
  if (copy_from_user(&cmd_mem, (int __user *)arg,sizeof(cmd_mem))){
    retval = -EFAULT; goto done; }
  //  printk(KERN_DEBUG "Get command: addr=%08x val=%08x\n",cmd_mem.addr, cmd_mem.val);

  switch(cmd){

  case SW_REG_READ:
    port=cmd_mem.port;
    address=CSR_BASE + port*CSR_SPAN + (cmd_mem.addr&0x0000003f);
    cmd_mem.val=ioread32(bar2top+address);
    rmb();
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }
#if VERB
    printk(KERN_DEBUG "IOR_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
#endif
    break;

  case SW_REG_WRITE:
    port=cmd_mem.port;
    address=CSR_BASE + port*CSR_SPAN + (cmd_mem.addr&0x0000003f);
    iowrite32(cmd_mem.val,bar2top+address);
    wmb();
#if VERB
    printk(KERN_DEBUG "IOW_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
#endif
    break;

  case SW_MEM_READ:
    if (cmd_mem.val>PCIE_BUFSIZ) real_len=PCIE_BUFSIZ;
    else real_len=cmd_mem.val;
    port=cmd_mem.port;
    address=DATA_BASE + DATA_SPAN*port; // + (cmd_mem.addr&0x0007ffff);
    if (real_len>0){
      if (!access_ok(VERIFY_WRITE, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      if (copy_to_user((int __user *)cmd_mem.ptr, bar0top+address, real_len)){
	retval = -EFAULT; goto done; }
      rmb();
#if VERB
      printk(KERN_DEBUG "IORB_cmd.size/addr %x %08x(%s)\n",real_len,address, __func__);
#endif
    }
    break;

  case SW_MEM_WRITE:
    if (cmd_mem.val>PCIE_BUFSIZ) real_len=PCIE_BUFSIZ;
    else real_len=cmd_mem.val;
    port=cmd_mem.port;
    address=DATA_BASE + DATA_SPAN*port; // + (cmd_mem.addr&0x0007ffff);
    real_len=cmd_mem.val;
    if (real_len>0){
      if (!access_ok(VERIFY_READ, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      if (copy_from_user(bar0top+address, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      wmb();
#if VERB
      printk(KERN_DEBUG "IOWB_cmd.size/address %x %08x(%s)\n",real_len,address,  __func__);
#endif
    }
    break;

  case SW_DMA_READ:
    port=cmd_mem.port;
    address=DMA_BASE+DATA_SPAN*port;
    if (cmd_mem.val>PCIE_BUFSIZ) real_len=PCIE_BUFSIZ;
    else real_len=cmd_mem.val;
    if (real_len>0){
      iowrite32(0x202,bar2top+AVMM_CSR);	//clear dma controler
      iowrite32(0x2,  bar2top+AVMM_CTL);	//reset dispatcher
      wmb();
      iowrite32(0x0,  bar2top+AVMM_CSR);	//clear all status
      iowrite32(0x10, bar2top+AVMM_CTL);	//set IRQ enable
      wmb();
      iowrite32(0x0,  bar2top+AVMM_CSR);	//clear all status (need?)
      wmb();
      data=ioread32(bar2top+AVMM_CSR);rmb();	//wait for desc buf not empty
      while((data&0x04)!=0){ data=ioread32(bar2top+AVMM_CSR); rmb();}
      //write desc, wqcond=0, and go
      phyaddr=(swp->dmaRbuf_phys&(~swp->a2p_mask));
      iowrite32(address,bar2top+AVMM_DES0);
      iowrite32(phyaddr,bar2top+AVMM_DES1);
      iowrite32(real_len,bar2top+AVMM_DES2);
      wqcond=0;
      wmb();
      iowrite32((1<<31)|(1<<14),bar2top+AVMM_DES3);
      //wait IRQ (remain_time:0=false at timeout, 1=true at timeout, or remain jiffies
      remain_time=wait_event_timeout(swp->wq,wqcond,TIMEOUT);
      //cheack IRQ status
#if VERB
      printk("wait IRQ: remain_time=%ld condition=%d irq_count=%d\n",
	     remain_time,wqcond,swp->irq_count);
#endif
      //copy to user space
      if (!access_ok(VERIFY_WRITE, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      if (copy_to_user((int __user *)cmd_mem.ptr, swp->dmaRbuf_virt, real_len)){
	retval = -EFAULT; goto done; }
#if VERB
      printk(KERN_DEBUG "IORD_cmd.size/address %x %08x(%s)\n",real_len,address, __func__);
#endif
    }
    break;

  case SW_DMA_WRITE:
    port=cmd_mem.port;
    address=DMA_BASE+DATA_SPAN*port;
    if (cmd_mem.val>PCIE_BUFSIZ) real_len=PCIE_BUFSIZ;
    else real_len=cmd_mem.val;
    if (real_len>0){
      //copy from user space
      if (!access_ok(VERIFY_READ, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      if (copy_from_user(swp->dmaWbuf_virt, (void __user *)cmd_mem.ptr,real_len)){
	retval = -EFAULT; goto done; }
      rmb();
      iowrite32(0x202,bar2top+AVMM_CSR);	//clear dma controler
      iowrite32(0x2,  bar2top+AVMM_CTL);	//reset dispatcher
      wmb();
      iowrite32(0x0,  bar2top+AVMM_CSR);	//clear all status
      iowrite32(0x10, bar2top+AVMM_CTL);	//set IRQ enable
      wmb();
      iowrite32(0x0,  bar2top+AVMM_CSR);	//clear all status (need?)
      wmb();
      data=ioread32(bar2top+AVMM_CSR);rmb();	//wait for desc buf not empty
      while((data&0x04)!=0){ data=ioread32(bar2top+AVMM_CSR); rmb();}
      //write desc, wqcond=0, and go
      phyaddr=(swp->dmaWbuf_phys&(~swp->a2p_mask))+swp->a2p_seg;
      iowrite32(phyaddr,bar2top+AVMM_DES0);
      iowrite32(address,bar2top+AVMM_DES1);
      iowrite32(real_len,bar2top+AVMM_DES2);
      wqcond=0;
      wmb();
      iowrite32((1<<31)|(1<<14),bar2top+AVMM_DES3);
      //wait IRQ
      remain_time=wait_event_timeout(swp->wq,wqcond,TIMEOUT);
      //cheack IRQ status
#if VERB
      printk("wait IRQ: remain_time=%ld condition=%d irq_count=%d\n",remain_time,wqcond,swp->irq_count);
      printk(KERN_DEBUG "IOWD_cmd.size/address %x %08x(%s)\n",real_len,address, __func__);
#endif
    }
    break;

  case SW_PCI_READ:
    address=cmd_mem.addr & 0x07ffffff;
    if (cmd_mem.addr & 0x80000000) cmd_mem.val=ioread32(bar0top+address);
    else                           cmd_mem.val=ioread32(bar2top+address);
    rmb();
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }
#if VERB
    printk(KERN_DEBUG "IORP_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
#endif
    break;

  case SW_PCI_WRITE:
    address=cmd_mem.addr & 0x07ffffff;
    if (cmd_mem.addr & 0x80000000) iowrite32(cmd_mem.val,bar0top+address);
    else                           iowrite32(cmd_mem.val,bar2top+address);
    wmb();
#if VERB
    printk(KERN_DEBUG "IOWP_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
#endif
    break;

  case SW_PCKT_READ:
    // RX CSR
    addr_rxcsr=CSR_BASE + cmd_mem.port*CSR_SPAN + ADD_RX_CSR;
    data=ioread32(bar2top+addr_rxcsr);
    // check buffer status
    if ((data&0x80000000)==0) {retval=-1; goto done;}
    if ((data&0x00400000)==0) {retval=-1; goto done;}
    // get size
    real_len=data&0x0fffff;
    get_size=(real_len+3)/4*4;
    // get data
    address=DATA_BASE + cmd_mem.port*DATA_SPAN;
    if (get_size>0){
      if (!access_ok(VERIFY_WRITE, (void __user *)cmd_mem.ptr,get_size)){
	retval = -EFAULT; goto done; }
      if (copy_to_user((int __user *)cmd_mem.ptr, bar0top+address, get_size)){
	retval = -EFAULT; goto done; }
      rmb();
      // flush buffer
      iowrite32(0,bar2top+addr_rxcsr);
      // return size
      cmd_mem.val=real_len;
      if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
	retval = -EFAULT; goto done; }
#if VERB
      printk(KERN_DEBUG "(%d)IORMR_cmd.size %08x (%s)\n",cmd_mem.port,real_len, __func__);
#endif
    }
    break;

  case SW_PCKT_WRITE:
    // get csr
    addr_txcsr=CSR_BASE + cmd_mem.port*CSR_SPAN + ADD_TX_CSR;
    data=ioread32(bar2top+addr_txcsr);
    // check buffer
    if ((data&0x80000000)!=0) {retval=-1; goto done;}
    // get size
    max_size=data&0x000ffffc;
    if (cmd_mem.val>max_size) put_size=max_size; else put_size=cmd_mem.val;
    real_len=(put_size+3)/4*4;
    // put data
    address=DATA_BASE + cmd_mem.port*DATA_SPAN;
    if (!access_ok(VERIFY_READ, (void __user *)cmd_mem.ptr,real_len)){
      retval = -EFAULT; goto done; }
    if (copy_from_user(bar0top+address, (void __user *)cmd_mem.ptr,real_len)){
      retval = -EFAULT; goto done; }
    wmb();
    // go!
    iowrite32(0x80400000+put_size,bar2top+addr_txcsr);
    //put_size to user
    cmd_mem.val=put_size;
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }

  case RMAP_REQ:
    // get csr
    addr_txcsr=CSR_BASE + cmd_mem.port*CSR_SPAN + ADD_TX_CSR;
    data=ioread32(bar2top+addr_txcsr);
    // check buffer
    if ((data&0x80000000)!=0) {retval=-1; goto done;}
    // get size
    max_size=data&0x000ffffc;
    if (cmd_mem.val>max_size) get_size=max_size; else get_size=cmd_mem.val;
    // create packet
    put_size=rmap_create_buffer(cmd_mem.cmd,cmd_mem.saddr,cmd_mem.daddr,
				cmd_mem.key,cmd_mem.tid,cmd_mem.addr,get_size,cmd_mem.val);
    real_len=(put_size+3)/4*4;
#if VERB
    printk(KERN_DEBUG "(%d)%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	   cmd_mem.port,
	   tx_buffer[0],tx_buffer[1],tx_buffer[2],tx_buffer[3],tx_buffer[4],tx_buffer[5],
	   tx_buffer[6],tx_buffer[7],tx_buffer[8],tx_buffer[9],tx_buffer[10],tx_buffer[11],
	   tx_buffer[12],tx_buffer[13],tx_buffer[14],tx_buffer[15]);
#endif
    address=DATA_BASE + cmd_mem.port*DATA_SPAN;
    memcpy_toio(bar0top+address,tx_buffer,real_len);
    wmb();
    // go !
    iowrite32(0x80400000+put_size,bar2top+addr_txcsr);
    cmd_mem.val=put_size;
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }
#if VERB
    printk(KERN_DEBUG "(%d)IOREQ_cmd.size %x (%s)\n",cmd_mem.port,real_len, __func__);
#endif
    break;

  case RMAP_RCV:
    // RX CSR
    addr_rxcsr=CSR_BASE + cmd_mem.port*CSR_SPAN + ADD_RX_CSR;
    data=ioread32(bar2top+addr_rxcsr);
    // check buffer status
    if ((data&0x80000000)==0) {retval=-1; goto done;}
    if ((data&0x00400000)==0) {retval=-1; goto done;}
    // get size
    max_size=cmd_mem.val;
    address=DATA_BASE + cmd_mem.port*DATA_SPAN;
    memcpy_fromio(buftop,bar0top+address,12);
#if VERB
    printk(KERN_DEBUG "(%d)%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	   cmd_mem.port,
	   buftop[0],buftop[1],buftop[2],buftop[3],buftop[4],buftop[5],
	   buftop[6],buftop[7],buftop[8],buftop[9],buftop[10],buftop[11]);
#endif
    cmd_mem.key=buftop[3];
    ret_tid=buftop[5]*0x100+buftop[6];
    cmd_mem.val=buftop[8]*0x10000+buftop[9]*0x100+buftop[10];
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }
    if (cmd_mem.tid!=0 && cmd_mem.tid!=ret_tid) {retval=-EFAULT;goto done;}
    if (cmd_mem.key!=0) {
      retval=-EFAULT;goto done;}
    get_size=(cmd_mem.val+3)/4*4;
    if (get_size>max_size) get_size=max_size;
    if (!access_ok(VERIFY_WRITE, (void __user *)cmd_mem.ptr,get_size)){
      retval = -EFAULT; goto done; }
    if (copy_to_user((int __user *)cmd_mem.ptr, bar0top+address+12, get_size)){
      retval = -EFAULT; goto done; }
    rmb();
    iowrite32(0,bar2top+addr_rxcsr);
#if VERB
    printk(KERN_DEBUG "(%d)IORCV_cmd.size %x (%s)\n",cmd_mem.port,get_size, __func__);
#endif
    break;

  case SW_TIME_MARK:
    printk(KERN_DEBUG "TimeMark(%4d): %lu/%d\n",cmd_mem.val,jiffies,HZ);
    break;

  default:
    retval = -ENOTTY;
    goto done;
    break;
  }

  done:
    return(retval);
}

/* swpci_probe() */
static int sw_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
  int rc = 0;
  struct swpci_dev *swp = NULL;
  u8 irq_pin, irq_line;
  
  printk(KERN_DEBUG DRV_NAME " probe(dev = 0x%p, pciid = 0x%p)\n", dev, id);

  /* allocate memory for per-board book keeping */
  swp = kzalloc(sizeof(struct swpci_dev), GFP_KERNEL);
  if (!swp) {
    printk(KERN_DEBUG "Could not kzalloc() for book keeping.\n");
    rc=-1;
    goto err_swp;
  }
  swp->pci_dev = dev;
  //  dev->dev.driver_data = (void *)swp;
  pci_set_drvdata(dev,swp);
  printk(KERN_DEBUG "probe() swp = 0x%p\n", swp);

  /* initialize spinlock */
  //  spin_lock_init(&swp->lock);

  /* enable device */
  rc = pci_enable_device(dev);
  if (rc) {
    printk(KERN_DEBUG "pci_enable_device() failed\n");
    goto err_enable;
  }

  pci_set_master(dev);/* enable bus master capability on device */
  rc = pci_enable_msi(dev);/* enable message signaled interrupts */
  if (rc) {  /* could not use MSI? */
    /* resort to legacy interrupts */
    printk(KERN_DEBUG "Could not enable MSI interrupting.\n");
    swp->msi_enabled = 0;
    /* MSI enabled, remember for cleanup */
  } else {
    printk(KERN_DEBUG "Enabled MSI interrupting.\n");
    swp->msi_enabled = 1;
  }

  pci_read_config_byte(dev, PCI_REVISION_ID, &swp->revision);
  //#if 0 /* example */
  if (swp->revision != REV_ID) {
    printk(KERN_DEBUG "Revision 0x%02x is not supported by this driver.\n",swp->revision);
    rc = -ENODEV;
    goto err_rev;
  }
  //#endif

  rc = pci_request_regions(dev, DRV_NAME);
  /* could not request all regions? */
  if (rc) {
    /* assume device is in use (and do not disable it later!) */
    swp->in_use = 1;
    goto err_regions;
  }
  swp->got_regions = 1;

  /* set DMA mask */
  if (!pci_set_dma_mask(dev, DMA_BIT_MASK(64))) {
    pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(64));
    /* use 64-bit DMA */
    printk(KERN_DEBUG "Using a 64-bit DMA mask.\n");
  } else if (!pci_set_dma_mask(dev, DMA_BIT_MASK(32))) {
    printk(KERN_DEBUG "Could not set 64-bit DMA mask.\n");
    pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(32));
    /* use 32-bit DMA */
    printk(KERN_DEBUG "Using a 32-bit DMA mask.\n");
  } else {
    printk(KERN_DEBUG "No suitable DMA possible.\n");
    /** @todo Choose proper error return code */
    rc = -1;
    goto err_mask;
  }

  /* interrupt */
  rc = pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq_pin);
  /* could not read? */
  if (rc) goto err_irq;
  printk(KERN_DEBUG "IRQ pin #%d (0=none, 1=INTA#...4=INTD#).\n", irq_pin);

  rc = pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq_line);
  /* could not read? */
  if (rc) {
    printk(KERN_DEBUG "Could not query PCI_INTERRUPT_LINE, error %d\n", rc);
    goto err_irq;
  }
  printk(KERN_DEBUG "IRQ line #%d(%d).\n", irq_line, dev->irq);
  irq_line = dev->irq;
  /* @see LDD3, page 259 */
  init_waitqueue_head(&swp->wq); wqcond=1;
  rc = request_irq(irq_line, sw_isr, IRQF_SHARED, DRV_NAME, (void *)swp);
  if (rc) {
    printk(KERN_DEBUG "Could not request IRQ #%d, error %d\n", irq_line, rc);
    swp->irq_line = -1;
    goto err_irq;
  }
  /* remember which irq we allocated */
  swp->irq_line = (int)irq_line;
  printk(KERN_DEBUG "Succesfully requested IRQ #%d with dev_id 0x%p\n", irq_line, swp);

  /* show BARs */
  scan_bars(swp, dev);
  /* map BARs */
  rc = map_bars(swp, dev);
  if (rc) goto err_map;

  /* PCIe IRQ mask */
  //  *((u32 *)swp->bar[2]+0x50/4)=0x0000ffff;
  iowrite32(0x0000ffff,swp->bar[2]+0x50);

  /* cdev */
  rc = swdev_init(swp);
  if (rc) goto err_cdev;

  printk(KERN_DEBUG "sw_probe() successful.\n");

  goto end;

 err_cdev:
  unmap_bars(swp,dev);
 err_map:
  /* free allocated irq */
  if (swp->irq_line >= 0)
    free_irq(swp->irq_line, (void *)swp);
 err_irq:;
 err_mask:
  if (swp->got_regions)
    pci_release_regions(dev);
 err_regions:;
 err_rev:
  if (swp->msi_enabled)
    pci_disable_msi(dev);
  /* disable the device if it is not in use */
  if (!swp->in_use)
    pci_disable_device(dev);
 err_enable:;
  if (swp)
    kfree(swp);
 err_swp:
 end:
  return rc;
}

/* sw_remove */
static void sw_remove(struct pci_dev *dev)
{
  struct swpci_dev *swp;
  printk(KERN_DEBUG "remove(0x%p)\n", dev);
  swp=pci_get_drvdata(dev);
  printk(KERN_DEBUG "remove(dev = 0x%p) driver_data = 0x%p\n", dev, swp);

  swdev_exit(swp);

  //  *((u32 *)swp->bar[2]+0x50/4)=0x00000000;
  iowrite32(0x00000000,swp->bar[2]+0x50);

  unmap_bars(swp, dev);

  if (swp->irq_line >= 0)
    free_irq(swp->irq_line, (void *)swp);
  if (swp->got_regions)
    pci_release_regions(dev);
  if (swp->msi_enabled){
    pci_disable_msi(dev);
    swp->msi_enabled=0;
  }

  if (!swp->in_use)
    pci_disable_device(dev);

  if (swp)
    kfree(swp);

  return;
}

/* character device file operations */
static struct file_operations swpci_fops = {
  .owner = THIS_MODULE,
  .open = swpci_open,
  .release = swpci_close,
  .unlocked_ioctl = swpci_ioctl,
  //  .read = swpci_read,
  //  .write = swpci_write,
};

/* used to register the driver with the PCI kernel sub system
 * @see LDD3 page 311
 */
static struct pci_driver sw_pci_driver = {
  .name = DRV_NAME,
  .id_table = ids,
  .probe = sw_probe,
  .remove = sw_remove,
  /* resume, suspend are optional */
};

/* swpci_init() */
static int __init swpci_init(void)
{
  int rc = 0;

  printk(KERN_DEBUG DRV_NAME "_init(), built at " __DATE__ " " __TIME__ "\n");
  printk(KERN_DEBUG DRV_NAME "_init(%lu/%d)\n",jiffies,HZ);
  printk(KERN_DEBUG DRV_NAME "_init(PAGESIZE=%ld)\n", PAGE_SIZE);
  printk(KERN_DEBUG DRV_NAME "_init(HZ=%d)\n", HZ);

  refcount=0;

  /* register this driver with the PCI bus driver */
  rc = pci_register_driver(&sw_pci_driver);

  if (rc < 0) {
    goto fail_pci;
  }
  return rc;

 fail_pci:
  return -1;
}

/* swdev_init */
static int swdev_init(struct swpci_dev *swp)
{
  int rc=0;
  dev_t devno = MKDEV(0,0);
  printk(KERN_DEBUG DRV_NAME " swdev_init()\n");
  swp->cdevno=devno;
  /* allocate a dynamically allocated character device node */
  rc = alloc_chrdev_region(&(swp->cdevno), 0/*requested minor base*/, DevsNum/*count*/, DRV_NAME);
  if (rc < 0) {
    printk("alloc_chrdev_region() = %d\n", rc);
    goto fail_alloc;
  }
  cdev_init(&swp->cdev, &swpci_fops);
  swp->cdev.owner = THIS_MODULE;
  rc = cdev_add(&swp->cdev, swp->cdevno, DevsNum/*count*/);
  if (rc < 0) {
    printk("cdev_add() = %d\n", rc);
    goto fail_add;
  }
  printk(KERN_DEBUG "swpci = %d:%d\n", MAJOR(swp->cdevno), MINOR(swp->cdevno));
  return 0;
 fail_add:
  unregister_chrdev_region(swp->cdevno,DevsNum);
 fail_alloc:
  return -1;
}

/* swpci_exit() */
static void __exit swpci_exit(void)
{
  printk(KERN_DEBUG DRV_NAME "_exit(%lu/%d)\n",jiffies,HZ);
  /* unregister this driver from the PCI bus driver */
  pci_unregister_driver(&sw_pci_driver);
}

/* swdev_exit */
static void swdev_exit(struct swpci_dev *swp)
{
  printk(KERN_DEBUG DRV_NAME " swdev_exit()\n");
  /* remove the character device */
  cdev_del(&swp->cdev);
  /* free the dynamically allocated character device node */
  unregister_chrdev_region(swp->cdevno, DevsNum/*count*/);
}

MODULE_LICENSE("Dual BSD/GPL");

module_init(swpci_init);
module_exit(swpci_exit);

unsigned char rmap_calc_crc(void *buf,unsigned int len){
  unsigned int i;
  unsigned char crc;
  unsigned char *ptr = (unsigned char *)buf;

  /* initial CRC */
  crc=0;

  /* for each bute */
  for(i=0;i<len;i++){
    crc=RM_CRCTbl[crc ^ *ptr++];
  }
  return crc;
}

int rmap_create_buffer(unsigned char command, unsigned char saddr, unsigned char daddr, unsigned char key,
		       unsigned int tid, unsigned int addr, unsigned int size, unsigned int data){// for "put" size is put_data
  unsigned int header_size;
  unsigned char *ptr, *dptr, *crc_start;
  
  ptr = tx_buffer;
  header_size=15;
  crc_start = ptr;
  *ptr++ = daddr;		//Destination Logical Address
  *ptr++ = 0x01;		//Protocol ID
  *ptr++ = command;		//Packet Type
  *ptr++ = key;			//Destination Key
  *ptr++ = saddr;		//Source Logical Address
  *ptr++ = (tid>>8)&0xff;	//Transaction ID(MS)
  *ptr++ = (tid   )&0xff;	//Transaction ID(LS)
  *ptr++ = 0x00;		//Extended Address
  *ptr++ = (addr>>24)&0xff;	//Address
  *ptr++ = (addr>>16)&0xff;	//Address
  *ptr++ = (addr>> 8)&0xff;	//Address
  *ptr++ = (addr    )&0xff;	//Address
  if ((command & 0x20)>0){ // if RM_PCKT_WRT
    *ptr++=0x00;		//Data Size
    *ptr++=0x00;		//Data Size
    *ptr++=0x04;		//Data Size
    *ptr++ = rmap_calc_crc(crc_start,header_size);	//Header CRC
    dptr=ptr;
    *ptr++ = (data    )&0xff;		//Data
    *ptr++ = (data>> 8)&0xff;		//Data
    *ptr++ = (data>>16)&0xff;		//Data
    *ptr++ = (data>>24)&0xff;		//Data
    *ptr++ = rmap_calc_crc(dptr,4);	//Data CRC
    return header_size + 6;
  }else{
    *ptr++ = (size>>16)&0xff;		//Data Size
    *ptr++ = (size>> 8)&0xff;		//Data Size
    *ptr++ = (size    )&0xff;		//Data Size
    *ptr++ = rmap_calc_crc(crc_start,header_size);	//Header CRC
    return header_size + 1;
  }
}
