#!/bin/bash

RES_DIR="$(pwd)/resources"

# u-boot related settings
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
else
    echo "Installation failed."
# revert changes
fi
