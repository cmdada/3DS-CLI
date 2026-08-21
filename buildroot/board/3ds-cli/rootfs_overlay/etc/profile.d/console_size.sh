#!/bin/sh
# Size the TTY to the console this is actually running on.
#
# The rootfs is identical on every console, but their terminals are not: 80x30
# on a 3DS, 160x60 on a Switch, 80x36 on a Wii. The emulator reports the grid
# it is drawing through the hw tree, so the size travels as data rather than
# being baked into the image. Without this the guest keeps the kernel's 80x24
# default and anything full-screen - vim, htop, nano - draws into a corner of
# a much larger screen.
[ -t 0 ] || return 0

_cs=/mnt/3ds/hw/console_size
[ -r "$_cs" ] || return 0

read -r _cols _rows < "$_cs" 2>/dev/null || return 0
case "$_cols$_rows" in
    *[!0-9]*|'') ;;                       # not two plain numbers: leave it alone
    *) stty cols "$_cols" rows "$_rows" 2>/dev/null ;;
esac
unset _cs _cols _rows
