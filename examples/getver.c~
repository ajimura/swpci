#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "swsoc_lib.h"

int main(int argc, char *argv[]) {
  int fd,ret;
  int ch;
  unsigned int data;
  fd=open("/dev/swsoc0",O_RDWR);

  sw_r(fd,0,ADD_CM_REG,&data);

  printf("Ver = %08X\n",data);

}
