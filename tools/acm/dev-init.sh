#!/bin/ash
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /proc /sys /dev /tmp /run /stock /dev/usb-ffs/acm
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
exec >/dev/console 2>&1
mkdir -p /dev/usb-ffs/acm /run/vf2
mount -t ext4 -o ro,noload /dev/nandd /stock
mount -t functionfs acm /dev/usb-ffs/acm
g=/sys/class/android_usb/android0
echo 0 > "$g/enable"
# Reuse the already installed WinUSB binding. RPC has its own ACM1 framing;
# no FEL tool is run while this development image is active.
echo 1f3a > "$g/idVendor"
echo efe8 > "$g/idProduct"
echo 'ACM VF2 RAM development' > "$g/iProduct"
echo 'ACM_VF2_DEV' > "$g/iSerial"
echo ffs > "$g/functions"
/bin/usb-rpc /dev/usb-ffs/acm > /tmp/usb-rpc.log 2>&1 &
sleep 2
echo 1 > "$g/enable"
echo 'VF2 USB development image ready. NAND mounted read-only.'
while :; do sleep 60; done
