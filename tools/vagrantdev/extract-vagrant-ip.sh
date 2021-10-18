#!/bin/bash

vagrantip=$(sudo -u $(logname) vagrant ssh -c "hostname -I" | grep -E '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}' | awk '{print $2}')
sudo sed -i "" "/vagrantdev/d" /etc/hosts &> /dev/null
sudo echo "$vagrantip vagrantdev" >> /etc/hosts
echo "inserted vagrantdev $vagrantip into /etc/hosts"