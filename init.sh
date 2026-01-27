#!/bin/sh

chmod +x /root/AKA-00/arm_init.sh
chmod +x /root/AKA-00/pwm_init.sh
chmod +x /root/AKA-00/https_init.sh

/root/AKA-00/arm_init.sh
/root/AKA-00/pwm_init.sh
/root/AKA-00/https_init.sh

nohup python3 /root/AKA-00/car_control_server.py > app.log 2>&1 &