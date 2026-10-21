#!/bin/bash
# piclick.sh X Y : move pointer to absolute X,Y and left-click
c(){ (echo "$1"; sleep 0.3) | timeout 2 nc -u -w1 ${AIOS_PI:?set AIOS_PI to the board ip} 42424 >/dev/null; }
c "mouse -3000 -3000"; c "mouse $1 $2"; c "mouse 0 0 1"; c "mouse 0 0 0"
