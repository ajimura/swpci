/* by Shuhei Ajimura */

#include <linux/ioctl.h>

#define DRV_NAME "swpci"
#define REV_ID 0x09

#define PCIE_BUFSIZ 0x4000

#define CSR_BASE  0x00006000
#define CSR_SPAN  0x00000100
#define DATA_BASE 0x00100000
#define DATA_SPAN 0x00010000
#define DMA_BASE  0x00100000

#define AVMM_CSR  0x00004000
#define AVMM_CTL  0x00004004
#define AVMM_DES0 0x00004020
#define AVMM_DES1 0x00004024
#define AVMM_DES2 0x00004028
#define AVMM_DES3 0x0000402C
#define AVMM_AD0L 0x00001000
#define AVMM_AD0H 0x00001004
#define AVMM_AD1L 0x00001008
#define AVMM_AD1H 0x0000100C

//struct swio_mem {
//  unsigned int addr;
//  unsigned int pad1;
//  unsigned int port;
//  unsigned int pad2;
//  unsigned int val;
//  unsigned int pad3;
//  unsigned int *ptr;
//};

struct swio_mem {
  unsigned int *ptr;
  unsigned int port;
  unsigned int addr;
  unsigned int val; //size for mem access
  unsigned int tid;
  char cmd;
  char saddr;
  char daddr;
  char key; // status for reply
};

#define IOC_MAGIC 's'

#define SW_REG_READ  _IOR(IOC_MAGIC, 1, struct swio_mem)
#define SW_REG_WRITE _IOW(IOC_MAGIC, 2, struct swio_mem)
#define SW_MEM_READ  _IOR(IOC_MAGIC, 3, struct swio_mem)
#define SW_MEM_WRITE _IOW(IOC_MAGIC, 4, struct swio_mem)
#define SW_DMA_READ  _IOR(IOC_MAGIC, 5, struct swio_mem)
#define SW_DMA_WRITE _IOW(IOC_MAGIC, 6, struct swio_mem)
#define SW_PCI_READ  _IOR(IOC_MAGIC, 7, struct swio_mem)
#define SW_PCI_WRITE _IOW(IOC_MAGIC, 8, struct swio_mem)

#define SW_PCKT_READ  _IOR(IOC_MAGIC, 9, struct swio_mem)
#define SW_PCKT_WRITE _IOW(IOC_MAGIC,10, struct swio_mem)
#define RMAP_REQ      _IOW(IOC_MAGIC,11, struct swio_mem)
#define RMAP_RCV      _IOR(IOC_MAGIC,12, struct swio_mem)
#define RMAP_RCV_DMA  _IOR(IOC_MAGIC,13, struct swio_mem)

#define SW_TIME_MARK  _IOW(IOC_MAGIC,14, struct swio_mem)

//#define SW_PCKT_READ  _IOR(IOC_MAGIC, 5, struct swio_mem)
//#define SW_PCKT_WRITE _IOW(IOC_MAGIC, 6, struct swio_mem)
//#define RMAP_REQ _IOW(IOC_MAGIC, 7, struct swio_mem)
//#define RMAP_RCV _IOR(IOC_MAGIC, 8, struct swio_mem)

#define ADD_CM_REG	0x00000000
#define ADD_ST_REG	0x00000004
#define ADD_CK_REG	0x00000008

#define ADD_RX_CSR	0x00000010
#define ADD_RX_DEBG	0x00000014

#define ADD_TX_CSR	0x00000020
#define ADD_TX_DEBG	0x00000024
