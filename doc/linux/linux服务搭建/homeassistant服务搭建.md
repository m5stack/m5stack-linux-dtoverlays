# Home-Assistant 服务安装

HomeAssistant是构建智慧空间的神器。是一个成熟完整的基于 Python 的智能家居系统，设备支持度高，支持自动化（Automation)、群组化（Group）、UI 客制化（Theme) 等等高度定制化设置。同样实现设备的 Siri 控制。基于HomeAssistant，可以方便地连接各种外部设备（智能设备、摄像头、邮件、短消息、云服务等，成熟的可连接组件有近千种），手动或按照自己的需求自动化地联动这些外部设备，构建随心所欲的智慧空间。HomeAssistant是开源的，它不属于任何商业公司，用户可以无偿使用。  

HomeAssistant 最初只是以 python 包的形式发布的，用户只需要执行 "pip install homeassistant" 就能完成安装，随着 HomeAssistant 的发展， HomeAssistant 变的越来越复杂，同时也包含了其他的服务，所以 HomeAssistant 的安装也变得复杂起来了。 

官方安装参考: [https://www.home-assistant.io/installation/](https://www.home-assistant.io/installation/)

## docker 安装

安装 docker ，可参考[docker安装](docker安装.md)
```bash
curl -fsSL get.docker.com | sh
``` 

安装 docker 镜像

arm64
``` bash
docker pull ghcr.io/home-assistant/aarch64-hassio-supervisor:2023.08.1
docker pull ghcr.io/home-assistant/aarch64-homeassistant:2023.8.1
```

x64
``` bash
docker pull ghcr.io/home-assistant/amd64-hassio-supervisor:2023.08.1
docker pull ghcr.io/home-assistant/qemux86-64-homeassistant:2023.8.1
```

启动

arm64
```bash
docker run -d --name hassio_supervisor --privileged \
--restart unless-stopped \
-v /var/run/docker.sock:/var/run/docker.sock \
-v /var/run/dbus:/var/run/dbus \
-v /opt/apps/hassio:/data \
-e SUPERVISOR_SHARE=/opt/apps/hassio \
-e SUPERVISOR_NAME=hassio_supervisor \
-e HOMEASSISTANT_REPOSITORY=homeassistant/aarch64-homeassistant \
ghcr.io/home-assistant/aarch64-hassio-supervisor:2023.08.1
```




x64
```bash
docker run -d --name hassio_supervisor --privileged \
--restart always \
-v /var/run/docker.sock:/var/run/docker.sock \
-v /var/run/dbus:/var/run/dbus \
-v /opt/apps/hassio:/data \
-e SUPERVISOR_SHARE=/opt/apps/hassio \
-e SUPERVISOR_NAME=hassio_supervisor \
-e HOMEASSISTANT_REPOSITORY=homeassistant/qemux86-64-homeassistant \
ghcr.io/home-assistant/amd64-hassio-supervisor:2023.08.1

```

然后需要耐心等待，按网速和性能需要蛮长时间的，用这个命令来看安装情况或者直接去portainer看log


```bash
docker logs -f hassio_supervisor
```


更新

当 homeassistant 版本有更新时，可以运行下面的命令进行更新。
```bash
# 这里的容器镜像名记住了替换下面的
docker stop hassio_supervisor
docker stop homeassistant
docker rm hassio_supervisor
docker rm homeassistant
docker images | grep supervisor
# 上面输出的都挨个删掉
docker rmi ghcr.io/home-assistant/amd64-hassio-supervisor:2022.04.0
docker images | grep 4-homeassistant
# 上面输出的都挨个删掉
docker rmi ghcr.io/home-assistant/qemux86-64-homeassistant:2022.3.6
# 这里下载最新版 删掉冒号后面的去访问就能知道最新版本是多少，替换掉
docker pull ghcr.io/home-assistant/amd64-hassio-supervisor:2023.08.1
docker pull ghcr.io/home-assistant/qemux86-64-homeassistant:2022.11.2
# 然后用上面安装的方式去运行，最后一行用上面pull的代替，SUPERVISOR_SHARE必须是同一个地方，迁移也只需要迁移他
# 万一 homeassistant 没有启动，那就执行下面这两条再执行上面安装，同时检查下下来的两个镜像是不是最新的稳定版本（没有dev字样）
docker stop hassio_cli hassio_multicast hassio_audio hassio_dns hassio_observer hassio_supervisor homeassistant
docker rm hassio_cli hassio_multicast hassio_audio hassio_dns hassio_observer hassio_supervisor homeassistant
```

安装 mqtt docker 服务
```bash
# 配置目录/etc/localtime
docker run -d --name emqx -v /etc/localtime:/etc/localtime -p 1883:1883 -p 18083:18083 emqx/emqx
```


## Home Assistant Core 安装

```bash
sudo apt-get update
sudo apt-get install -y python3 python3-dev python3-venv python3-pip bluez libffi-dev libssl-dev libjpeg-dev zlib1g-dev autoconf build-essential libopenjp2-7 libtiff6 libturbojpeg0-dev tzdata ffmpeg liblapack3 liblapack-dev libatlas-base-dev

sudo useradd -rm homeassistant

sudo mkdir /srv/homeassistant
sudo chown homeassistant:homeassistant /srv/homeassistant

sudo -u homeassistant -H -s
cd /srv/homeassistant
python3 -m venv .
source bin/activate

python3 -m pip install wheel

pip3 install homeassistant==2023.10.5

hass
```
安装完成后访问 http://homeassistant.local:8123 。