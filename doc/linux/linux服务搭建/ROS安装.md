# ROS 安装搭建

ROS （Robot Operating System）起源于2007年斯坦福大学人工智能实验室与WillowGarage公司的个人机器人项目，其后被Willow Garage公司开源和发展，目前由OSRF（Open Source Robotics Foundation, Inc）公司维护。它是一个开源的面向机器人软件开发的灵活框架，是一系列开发工具和开发库的集合体，同时，作为一种类似于传统操作系统的元操作系统（Meta-Operating System）还提供了硬件抽象、设备驱动、信息传递等诸多功能。


## ROS1 安装

官方wiki教程链接：
[http://wiki.ros.org/melodic/Installation/Ubuntu](http://wiki.ros.org/melodic/Installation/Ubuntu)


```bash
sudo sh -c 'echo "deb http://packages.ros.org/ros/ubuntu $(lsb_release -sc) main" > /etc/apt/sources.list.d/ros-latest.list'

sudo apt-key adv --keyserver 'hkp://keyserver.ubuntu.com:80' --recv-key C1CF6E31E6BADE8868B172B4F42ED6FBAB17C654

sudo apt update

sudo apt install ros-melodic-desktop-full

sudo rosdep init

rosdep update

```


## ROS2 安装

官方wiki教程链接：
[http://wiki.ros.org/noetic/Installation/Ubuntu](http://wiki.ros.org/noetic/Installation/Ubuntu)

```bash
sudo sh -c 'echo "deb http://packages.ros.org/ros/ubuntu $(lsb_release -sc) main" > /etc/apt/sources.list.d/ros-latest.list'

sudo apt install curl # if you haven't already installed curl
curl -s https://raw.githubusercontent.com/ros/rosdistro/master/ros.asc | sudo apt-key add -

sudo apt update

sudo apt install ros-noetic-desktop-full

sudo apt install rospack-tools

sudo rosdep init

rosdep update

```













