#! /bin/bash

set -e

sudo insmod ../kpub.ko
echo -n counter > /sys/class/kpub/create_topic
echo -ne '\x04' > '/sys/class/kpub/kpub!counter/msg_size'
echo -ne '\x02' > '/sys/class/kpub/kpub!counter/msg_count'

