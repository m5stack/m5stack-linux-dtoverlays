
Note that the aw882xx_drv.ko file was compiled on 32-bit systems and can only be loaded on 32-bit systems. This file cannot be loaded using the `sudo depmod - a&&sudo modprobe aw882xx_drv` command. Only the `insmod aw882xx_drv.ko` command can be loaded. Otherwise, an irreversible error will be generated. This makefile only provides an installer, not a launcher. Please use the boot file to load.

Please use the boot script to load the kernel file. Please refer to the following to write the boot script:

# systemd Starts the system and runs rc.local  

1、Modify the content of `/lib/systemd/system/rc-local.service` as follows, mainly by adding the Install field.
``` bash
#  SPDX-License-Identifier: LGPL-2.1+
#
#  This file is part of systemd.
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.

# This unit gets pulled automatically into multi-user.target by
# systemd-rc-local-generator if /etc/rc.local is executable.
[Unit]
Description=/etc/rc.local Compatibility
Documentation=man:systemd-rc-local-generator(8)
ConditionFileIsExecutable=/etc/rc.local
After=network.target

[Service]
Type=forking
ExecStart=/etc/rc.local start
TimeoutSec=0
RemainAfterExit=yes
GuessMainPID=no

[Install]
WantedBy=multi-user.target
```

2. Modify the contents of `/etc/rc.local` as follows. If no, create it

``` bash
#!/bin/bash
insmod /usr/local/modules/aw882xx_drv.ko
exit 0
```

Also, don't forget to add executable permissions to `/etc/rc.local`

``` bash
sudo chmod +x /etc/rc.local
```

Then execute:
``` bash
sudo systemctl enable rc-local

# Then start the service and see its status

sudo systemctl start rc-local.service
sudo systemctl status rc-local.service
```

The command output is as follows

``` bash
● rc-local.service - /etc/rc.local Compatibility
   Loaded: loaded (/etc/systemd/system/rc-local.service; enabled; vendor preset: enabled)
  Drop-In: /lib/systemd/system/rc-local.service.d
           └─debian.conf
   Active: active (running) since Thu 2018-11-01 13:17:08 CST; 2s ago
     Docs: man:systemd-rc-local-generator(8)
  Process: 10810 ExecStart=/etc/rc.local start (code=exited, status=0/SUCCESS)
    Tasks: 1 (limit: 4915)
   CGroup: /system.slice/rc-local.service
           └─10811 /usr/bin/python /usr/bin/sslocal -c /home/xugaoxiang/Tools/ss/ss.json

11月 01 13:17:08 ubuntu systemd[1]: Starting /etc/rc.local Compatibility...
11月 01 13:17:08 ubuntu systemd[1]: Started /etc/rc.local Compatibility.
11月 01 13:17:08 ubuntu rc.local[10810]: INFO: loading config from /home/xugaoxiang/Tools/ss/ss.json
11月 01 13:17:08 ubuntu rc.local[10810]: 2018-11-01 13:17:08 INFO     loading libcrypto from libcrypto.so.1.1
11月 01 13:17:08 ubuntu rc.local[10810]: 2018-11-01 13:17:08 INFO     starting local at 127.0.0.1:1080
```

You can see that the script in rc.local has been executed correctly.
