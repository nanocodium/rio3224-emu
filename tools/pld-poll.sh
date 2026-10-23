#!/bin/bash
# pld-poll.sh : which PLD registers the firmware polls once it is running (first 400 PLD accesses after 24 s)
cd /mnt/d/rio3224-emu/emu
printf '24000 ;mark\n' > /tmp/pldpoll-console.txt
cp ../fw/d2/flash.bin /tmp/pldpoll-flash.bin
~/rio3224/build/rio-emu --max 4700000000 --no-raw --flash /tmp/pldpoll-flash.bin --dante-boot-ms 15000 --log PLD \
    --console-script /tmp/pldpoll-console.txt < /dev/null > /dev/null 2> /tmp/pldpoll.err
grep -A400 -m1 'console. 24000' /tmp/pldpoll.err | grep '^\[PLD' | sed -E 's/ (->|<-) .*//' | sort | uniq -c | sort -rn | head -20
echo "--- sample sequence:"
grep -A60 -m1 'console. 24000' /tmp/pldpoll.err | grep '^\[PLD' | head -40 | cut -c1-80
