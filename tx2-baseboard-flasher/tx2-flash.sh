#!/bin/bash

RES_DIR="$(pwd)/resources"
BOOT_DIR="/boot"

# fw_env related settings
ENV_DEV="/dev/mmcblk0boot1"
ENV_OFFSET="003fe000"
ENV_SIZE="0x2000"
ENV_SECT="0x1000"

# u-boot related settings
UB_FDT_FILE="tegra186-quill-p3310-1000-c03-00-base.dtb"
UB_KERNEL_FILE="Image"
UB_BSCRIPT_FILE="bootscr.img"
MOTD_FILE="motd"
MOTD_SCRIPT="10-motd"
UB_BOOTCMD="load mmc 0 \${scriptaddr} boot/\${bootscr_file} && source \${scriptaddr}"
UB_ENV_FILE="uboot-minimal-env.env"
INST_STATUS=0

# misc settings
DEFAULT_WALLPAPER="AM-wallpaper-landscape.png"

# check for root
if [[ $EUID -ne 0 ]];
then
    echo "Root privilages are required. Try:"
    echo "sudo $0"
    exit 1
fi

# check if force update
force_update=false
if [ $# -eq 1 ] && [ "$1" == "--force-update" ]; then
    force_update=true
fi

# check if we are runnig for the first time
if [ -d $BOOT_DIR/boot-jetpack-orig ]; then
    echo "TX2 seems to be flashed already."
    if [ "$force_update" = false ]; then
        echo "Please run $0 --force-update if you want to overwrite the current configuration."
        exit 1
    else
        echo "Running with force-update enabled."
    fi
else
# running for the first time - create temp archive - we will rename it on success
    cp -r $BOOT_DIR /tmp/boot-jetpack-orig
fi

# install u-boot-tools (provides fw_setenv/fw_getenv)
echo "Updating apt sources..."
apt-get update &>/dev/null
if ! apt-get -y install u-boot-tools
then
    echo "Unable to install u-boot-tools. Exiting..."
    exit 1
fi

# install device tree compiler
if ! apt-get -y install device-tree-compiler
then
    echo "Unable to install device-tree-compiler. Exiting..."
    exit 1
fi

#install update-motd
if ! apt-get -y install update-motd
then
    echo "Unable to install update-motd. Exiting..."
    exit 1
fi

#install libncurses5-dev (used e.g. by make menuconfig)
if ! apt-get -y install libncurses5-dev
then
    echo "Unable to install libncurses5-dev. Exiting..."
    exit 1
fi


# create fw_env config
echo -e "$ENV_DEV\t$ENV_OFFSET\t$ENV_SIZE\t$ENV_SECT" > /etc/fw_env.config

# make fw_setenv able to write to boot partition
echo 0 > /sys/block/mmcblk0boot1/force_ro
# here we have to perform saveenv operation - fake it for now
echo "Updating env..."
fw_setenv fdt_file "$UB_FDT_FILE"
fw_setenv kernel_file "$UB_KERNEL_FILE"
fw_setenv bootcmd "$UB_BOOTCMD"
fw_setenv bootscr_file "$UB_BSCRIPT_FILE"
fw_setenv -s "$RES_DIR/$UB_ENV_FILE"
# lock the boot partition
echo 1 > /sys/block/mmcblk0boot1/force_ro

# remove original files
if [ "$force_update" = false ]; then
    rm -rf $BOOT_DIR
    mkdir $BOOT_DIR
fi

echo "echo" > bootscr
cat $RES_DIR/motd | tr  -d '\r' | sed -e 's/`/"\\`"/g' | sed -e "s/'/\"\\\'\"/g" |  awk '{ print "echo \""$0"\"" }' > bootscr
cat $RES_DIR/boot.scr >> bootscr
mkimage -A arm64 -T script -C none -a 0 -e 0 -n 'U-Boot TX2 bootscript' -d bootscr $BOOT_DIR/$UB_BSCRIPT_FILE

# update devicetree
echo "Copying devicetree..."
# create .old backup
if [ -f $BOOT_DIR/$UB_FDT_FILE ]; then
    cp $BOOT_DIR/$UB_FDT_FILE $BOOT_DIR/$UB_FDT_FILE.old
fi

if ! cp "$RES_DIR/$UB_FDT_FILE" "$BOOT_DIR/$UB_FDT_FILE"
then
    INST_STATUS=1
    echo "Unable to update the devicetree."
fi

# update kernel
echo "Copying kernel..."
# create .old backup
if [ -f $BOOT_DIR/$UB_KERNEL_FILE ]; then
    cp $BOOT_DIR/$UB_KERNEL_FILE $BOOT_DIR/$UB_KERNEL_FILE.old
fi

if ! cp "$RES_DIR/$UB_KERNEL_FILE" "$BOOT_DIR/$UB_KERNEL_FILE"
then
    INST_STATUS=1
    echo "Unable to update the kernel image."
fi

# update modules
echo "Updating kernel modules..."
if [ -d "/lib/modules/4.4.38-antmicro" ]; then
    rm -r "/lib/modules/4.4.38-antmicro"
fi
if ! cp -r "$RES_DIR/modules-4.4.38-antmicro" "/lib/modules/4.4.38-antmicro"
then
    INST_STATUS=1
    echo "Unable to update kernel modules."
fi

# update password
echo "root:root" | chpasswd
# allow root login
sed -i "s/PermitRootLogin prohibit-password/PermitRootLogin yes/g" /etc/ssh/sshd_config

echo "Setting custom motd..."
if [ -d "/boot/motd" ]; then
   rm -r "/boot/motd"
fi
if ! cp "$RES_DIR/$MOTD_FILE" "/boot/motd"
then
   INST_STATUS=1
   echo "Unable to insert motd."
fi
if [ -d "/etc/update-motd.d/$MOTD_SCRIPT" ]; then
   rm -r "/etc/update-motd.d/$MOTD_SCRIPT"
fi
if ! cp "$RES_DIR/$MOTD_SCRIPT" "/etc/update-motd.d/$MOTD_SCRIPT"
then
   INST_STATUS=1
   echo "Unable to insert motd."
fi
# set permissions
chmod a+x /etc/update-motd.d/$MOTD_SCRIPT

# set hostname
echo "antmicro-tx2-baseboard" > /etc/hostname
echo -n -e "127.0.0.1 localhost\n127.0.0.1 antmicro-tx2-baseboard\n" > /etc/hosts

# copy default wallpaper
if ! cp "$RES_DIR/$DEFAULT_WALLPAPER" "/usr/share/backgrounds"
then
    INST_STATUS=1
    echo "Unable to copy file $DEFAULT_WALLPAPER"
else
    DISPLAY=:0.0 gsettings set org.gnome.desktop.background picture-uri file:///usr/share/backgrounds/$DEFAULT_WALLPAPER
fi

# update linux-headers
echo "Updating linux-headers..."
if ! cp -r "$RES_DIR/linux-headers-4.4.38-antmicro" "/usr/src/"
then
    INST_STATUS=1
    echo "Unable to copy linux-headers-4.4.38-antmicro"
fi

sync

if [[ $INST_STATUS -eq 0 ]]; then
    echo "Installation finished. You can now reboot the board."
    if [ "$force_update" = false ]; then
        mv /tmp/boot-jetpack-orig $BOOT_DIR/boot-jetpack-orig
    fi
else
    echo "Installation failed."
# revert changes
    if [ "$force_update" = false ]; then
        rm -rf $BOOT_DIR
        mv /tmp/boot-jetpack-orig $BOOT_DIR
    fi
fi
