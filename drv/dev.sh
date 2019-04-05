#!/bin/sh
mknod /dev/swpci c  $(awk "\$2==\"swpci\" {print \$1}" /proc/devices) 0
chmod 0666 /dev/swpci*
