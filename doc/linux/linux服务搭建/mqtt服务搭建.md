# MQTT
MQTT(消息队列遥测传输)是ISO 标准(ISO/IEC PRF 20922)下基于发布/订阅范式的消息协议。它工作在TCP/IP协议族上，是为硬件性能低下的远程设备以及网络状况糟糕的情况下而设计的发布/订阅型消息协议，为此，它需要一个消息中间件。  
IBM公司的安迪·斯坦福-克拉克及Cirrus Link公司的阿兰·尼普于1999年撰写了该协议的第一个版本。  
该协议的可用性取决于该协议的使用环境。IBM公司在2013年就向结构化资讯标准促进组织提交了 MQTT 3.1 版规范，并附有相关章程，以确保只能对规范进行少量更改。MQTT-SN是针对非 TCP/IP 网络上的嵌入式设备主要协议的变种，与此类似的还有ZigBee协议。  
纵观行业的发展历程，“MQTT”中的“MQ” 是来自于IBM的MQ系列消息队列产品线。然而通常队列本身不需要作为标准功能来支持。  
可选协议包含了高级消息队列协议，面向文本的消息传递协议，互联网工程任务组约束应用协议，可扩展消息与存在协议，数据分发服务，OPC UA以及web 应用程序消息传递协议。   
轻量的协议帧，中心式通信等优点让 mqtt 成为物联网的基础协议之一。



## Mosquitto

Mosquitto 是一个广泛使用的开源 MQTT Broker，隶属于 Eclipse 基金会，遵循 Eclipse 公共许可证（EPL/EDL 许可证）。截至 2023 年 3 月，它在 GitHub 上拥有超过 7k 个 Star。Mosquitto 支持 MQTT 5.0、3.1.1、3.1，并且提供了对 SSL/TLS 和 WebSocket 的支持。

Mosquitto 由 C/C++ 编写，采用单线程架构。其轻量级设计使得它非常适合在资源受限的嵌入式设备或工业网关上部署。Mosquitto 是跨平台的，可以在包括 Linux、Windows、macOS 在内的多种平台上运行。
### 优点
- 轻量级、占用资源少
- 简单易用

### 缺点

- 不支持多线程和集群
- 不支持在云端部署

### 应用场景
- 工厂自动化
- 智能制造
- 智能硬件

### 安装部署

1、安装需要的软件包
```bash
# 更新apt的资源列表
sudo apt-get update

# 安装mosquitto 和mosquitto-clients
sudo apt-get install mosquitto mosquitto-clients

```
2、配置 mqtt 服务器
```bash
# 查看配置文件示例
cat /usr/share/doc/mosquitto/examples/mosquitto.conf


# 编辑配置文件

sudo nano /etc/mosquitto/mosquitto.conf

# 添加下面的内容

```

``` text
# mosquitto 服务器用户
user mosquitto

# 最大缓存消息数 200
max_queued_messages 200

# 运行 0 长连接 id
allow_zero_length_clientid true

# mqtt 服务端口 1883
listener 1883

# 不允许匿名用户登陆
allow_anonymous false

# mqtt 登陆用户名和密码存放文件
password_file /etc/mosquitto/passwd.conf
```
```bash
# 生成账号密码
# 下面两种方法选一种。
# 密文创建账户

sudo mosquitto_passwd -c /etc/mosquitto/passwd.conf 用户名
# 输入两遍密码

# 例如：我们的用户名为ct
sudo mosquitto_passwd -c /etc/mosquitto/passwd.conf ct


# 明文创建账户
sudo mosquitto_passwd -b /etc/mosquitto/passwd.conf 用户名 密码
# 一般情况不用明文账户。


# 重启 mqtt 服务，使配置能够生效
systemctl restart mosquitto.service

# 查看 mqtt 服务运行状态。
systemctl status mosquitto.service


# 测试订阅 topic/demo
mosquitto_sub -h localhost -t topic/test -u user -P password

# 测试发布
mosquitto_pub -h localhost -t topic/test -u user -P password -m "Hello, World!"
# 注意替换你的 user 和 password 。
```





## EMQX

EMQX 是一款高度可扩展的分布式 MQTT Broker，适用于企业级的工业物联网部署。它支持 MQTT 5.0、MQTT-SN、SSL/TLS、MQTT over QUIC 等多种协议。它通过 masterless 集群方式实现了高可用性和水平扩展性。

凭借在 GitHub 上的 11.5k 个 Star，EMQX 已经成为市场上最受欢迎的 MQTT Broker 之一。EMQX 项目于 2012 年启动，采用 Apache 2.0 许可证进行开源。EMQX 由 Erlang/OTP 编写，这是一种能够构建高度可扩展的软实时系统的编程语言。

EMQX 既可以在云端部署，也可以在边缘部署。在边缘，它可以与各种工业网关集成，例如 N3uron、Neuron、Kepware。在云环境中，EMQX 能够在 AWS、GCP、Azure 等主流的公共云平台上与包括 Kafka、数据库和云服务在内的多种技术无缝集成。

借助全面的企业级功能、数据集成能力、云托管服务和 EMQ 团队提供的商业支持，EMQX 广泛应用于工业物联网领域的多种重要场景。

### 优点
- Masterless 集群和高可用性
- 具有高性能和低延迟
- 提供丰富的认证机制
- 即可以在边缘部署也可以在云端部署
- 首个支持 MQTT over QUIC 的 MQTT Broker

### 缺点
- 安装和配置相对复杂
- CPU 和内存使用率较高

### 应用场景
- 汽车制造
- 钢铁制造
- 石油和天然气
- 半导体制造
- 供水

### 安装部署
``` bash
# 在 EMQX 的 github 主页：https://github.com/emqx/emqx 中的 releases 中下载对应的软件版本。或者参考 https://www.emqx.io/docs/en/v5.1/deploy/install.html 官网安装指南。以树梅派64位为例子。
# Download emqx-5.1.6-debian10-arm64.deb 
wget https://www.emqx.com/en/downloads/broker/5.1.6/emqx-5.1.6-debian10-arm64.deb

# Install EMQX 
sudo apt install ./emqx-5.1.6-debian10-arm64.deb

# Run EMQX 
sudo systemctl start emqx
```

更多安装参考 https://www.emqx.io/downloads。

## NanoMQ

NanoMQ 是一个最新的开源 MQTT Broker 项目，于 2020 年发布。它采用纯 C 语言编写，基于 NNG 的异步 I/O 多线程 Actor 模型，支持 MQTT 3.1.1、MQTT 5.0、SSL/TLS、MQTT over QUIC。

NanoMQ 的突出亮点是轻量级、快速、极低的内存占用，这使它成为一款在工业物联网中表现非常优秀的 MQTT Broker，因为在工业物联网中效率和资源优化非常重要。此外，NanoMQ 还可以用作消息总线，将 DDS、NNG、ZeroMQ 等协议转换为 MQTT，然后再将 MQTT 消息桥接到云端。

NanoMQ 具有高度的兼容性和可移植性，只依赖于原生的 POSIX API。这使得它可以轻松地部署在任何支持 POSIX 标准的平台上，并且能够在 x86_64、ARM、MIPS、RISC-V 等各种 CPU 架构上顺畅运行。
### 优点
- 支持多线程和异步 IO
- 启动占用资源少
- 可以与无代理协议桥接

### 缺点
- 项目还处于早期阶段
- 不支持集群

### 应用场景
- 汽车制造
- 机器人：边缘服务融合
- 工业物联网边缘网关
```bash

```

### 安装部署
``` bash
# 在 EMQX 的 github 主页：https://github.com/emqx/nanomq 中的 releases 中下载对应的软件版本。或者参考 https://nanomq.io/docs/zh/latest/installation/packages.html 官网安装指南。以树梅派 64 位为例子。
# 下载 NanoMQ 仓库
curl -s https://assets.emqx.com/scripts/install-nanomq-deb.sh | sudo bash

# 安装 NanoMQ
sudo apt-get install nanomq

# 启动 NanoMQ
nanomq start  

```



https://nanomq.io/docs/zh/latest/






# 建议
文章提供了三种 mqtt 服务器的简介与安装。推荐使用 Mosquitto， Mosquitto 用户数比较多，文档和教程也较多。


# 参考
[1] Mosquitto 官网, https://mosquitto.org/  
[2] EMQX 官网, https://www.emqx.io/zh  
[3] NanoMQ 官网 ，https://nanomq.io/docs/zh/  latest/