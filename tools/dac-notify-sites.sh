#!/bin/bash
# dac-notify-sites.sh : every dacCmdSendNotify call in dac with the function name and the nearest preceding "mov r0, #N" (the notify command id)
awk '/mov\tr0, #/ {last=$0} /bl\t0x236e0 <dacCmdSendNotify>/ { match($0, /<[A-Za-z0-9_]+\+/); fn=substr($0, RSTART+1, RLENGTH-2); match(last, /#[0-9]+/); print fn, substr(last, RSTART) }' /tmp/dac_text.txt | sort | uniq -c
