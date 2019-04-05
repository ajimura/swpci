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

/* read and write BAR-mem */
static void read_bar(u32 *bar, u32 address, u32 *data)
{
  *data=ioread32(bar+address/4);
  //  *data=*(bar+address/4);
}
static void write_bar(u32 *bar, u32 address, u32 data)
{
  iowrite32(data,bar+address/4);
  //  *(bar+address/4)=data;
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
  unsigned int phyaddr;
  unsigned int port;
  int real_len;
  unsigned int data;
  long remain_time;

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
    //    cmd_mem.val=*(bar2top+(address)/4);
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
    //    *(bar2top+(address)/4)=cmd_mem.val;
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
      printk("wait IRQ: remain_time=%ld condition=%d irq_count=%d\n",
	     remain_time,wqcond,swp->irq_count);
      printk(KERN_DEBUG "IOWD_cmd.size/address %x %08x(%s)\n",real_len,address, __func__);
#endif
    }
    break;

  case SW_PCI_READ:
    address=cmd_mem.addr & 0x07ffffff;
    if (cmd_mem.addr & 0x80000000)
      //      cmd_mem.val=*(bar0top+(address)/4);
      cmd_mem.val=ioread32(bar0top+address);
    else
      //      cmd_mem.val=*(bar2top+(address)/4);
      cmd_mem.val=ioread32(bar2top+address);
    rmb();
    if (copy_to_user((int __user *)arg, &cmd_mem, sizeof(cmd_mem))){
      retval = -EFAULT; goto done; }
#if VERB
    printk(KERN_DEBUG "IORP_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
#endif
    break;

  case SW_PCI_WRITE:
    address=cmd_mem.addr & 0x07ffffff;
    if (cmd_mem.addr & 0x80000000)
      //      *(bar0top+(address)/4)=cmd_mem.val;
      iowrite32(cmd_mem.val,bar0top+address);
    else
      //      *(bar2top+(address)/4)=cmd_mem.val;
      iowrite32(cmd_mem.val,bar2top+address);
    wmb();
#if VERB
    printk(KERN_DEBUG "IOWP_cmd.val/addr %08x %08x(%s)\n",cmd_mem.val,address, __func__);
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
