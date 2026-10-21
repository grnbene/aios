#!/bin/bash
# pishot.sh NAME -> /tmp/ui/NAME.png (full-res from tcp/42427)
mkdir -p /tmp/ui
nc -w 20 ${AIOS_PI:?set AIOS_PI to the board ip} 42427 > /tmp/ui/$1.qoi && python3 /repos/aios/metal/qoi2png.py /tmp/ui/$1.qoi /tmp/ui/$1.png && echo /tmp/ui/$1.png
