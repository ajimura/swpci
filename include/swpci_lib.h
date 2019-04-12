#include <linux/ioctl.h>

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
/* changed on May/30 midnight
  REG_WR  : addr=reg address, port=NA, val=data, ptr=N.A.
  REG_RD  : addr=reg address, port=NA, val=data, ptr=N.A.
  MEM_WR  : addr=NA, port=port, val=size, ptr=data buffer
  MEM_RD  : addr=NA, port=port, val=size, ptr=data buffer
  DMA_WR  : addr=cmd id, port=NA, val=NA, ptr=NA
  DMA_RD  : addr=cmd id, port=NA, val=NA, ptr=NA
*/

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

#define DEVFILE "/dev/swpci"

#define CSR_BASE  0x00006000
#define CSR_SPAN  0x00000100
#define DATA_BASE 0x00000000
#define DATA_SPAN 0x00010000

#define ADD_CM_REG	0x00000000
#define ADD_ST_REG	0x00000004
#define ADD_CK_REG	0x0000000C

#define ADD_RX_CSR	0x00000010
#define ADD_RX_DEBG	0x00000014

#define ADD_TX_CSR	0x00000020
#define ADD_TX_DEBG	0x00000024

int sw_open(int);
void sw_close(int);
int sw_w(int, int, unsigned int, unsigned int);
int sw_r(int, int, unsigned int, unsigned int *);
int sw_bw(int, int, unsigned int *, unsigned int);
int sw_br(int, int, unsigned int *, unsigned int);
int sw_put_data0(int, int, unsigned int *, unsigned int);
int sw_put_data(int, int, unsigned int *, unsigned int);
int sw_get_data0(int, int, unsigned int *, unsigned int);
int sw_get_data(int, int, unsigned int *, unsigned int);
int sw_put_dma(int, int, unsigned int *, unsigned int);
int sw_get_dma(int, int, unsigned int *, unsigned int);
int sw_req(int, int, int, int, int, int, int, int, int);
int sw_rcv(int, int, unsigned int *, int *, int, int);
int sw_drcv(int, int, unsigned int *, int *, int, int);
int sw_link_test(int, int);
int sw_link_check(int , int);
void sw_link_reset(int, int);
void sw_link_up(int, int);
void sw_link_down(int, int);
int sw_rx_status(int, int);
int sw_rx_flush(int, int);
int sw_wait_data(int, int);
void sw_print_status(int, int);
