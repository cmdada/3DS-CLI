#!/bin/sh
# Run neofetch on every interactive login shell.
if [ -t 0 ] && command -v neofetch >/dev/null 2>&1; then
    neofetch
fi
