# cm4stack atomS3 sensor interaction for nodered
本文简述一个 cm4stack linux 平台和 atomS3 mcu 平台联动，使用 Node-RED，进行简单互动的小项目搭建应用示例。为了更加简化部署安装，我们同时提供 docker 形式一键安装启动。

## 简介
cm4stack 是 M5 公司一款基于 raspberry cm4 计算平台的小尺寸微型计算机，安装 linux 系统。具有可开发硬件同时，也具有硬件 io 接口属性。在本项目中承载服务平台。

atomS3 是 M5 公司一款基于 ESP32 S3 MCU 的小尺寸单片机主控平台。能够连接 wifi 网络的同时，驱动小型传感器执行器等。在本平台内承担传感器数据上传和执行执行的任务。

Node-RED是IBM公司开发的一个可视化的编程工具，以满足他们快速连接硬件和设备到Web服务和其他软件的需求，很快发展成为一种通用的物联网编程工具。




## 准备
系统安装参考 [cm4系统安装](../cm4stack/安装系统指南.md)  
开机配置参考 [首次开机指南](../cm4stack/首次开机指南.md)  
mqtt服务安装参考 [mqtt服务搭建](./linux服务搭建/mqtt服务搭建.md)  
Node-RED 服务安装参考 [Node-RED服务搭建](./linux服务搭建/nodered服务搭建.md)  

## 使用 uifow 编写传感器程序


## 使用 uiflow 编写继电器程序


## 使用 nodered 编写控制


