# docker 安装

此处只分享docker CE安装方法，商业版多了容器资源监控和镜像扫描的功能，不考虑教程提供。

有三种安装办法：

- 在软件源内添加docker，并且安装，这样安装和升级比较方便，官方推荐。  
- 下载离线包手动安装，手动升级，一般在与互联网隔绝的情况下使用此方法。  
- 官方一键安装脚本。

## 脚本安装

直接执行官方出品的一键[安装脚本](https://github.com/docker/docker-install)（脚本会区分不同的操作系统且脚本会安装体验版（edge版）而不是稳定版（stable版）。


```bash
$ sudo wget -qO- https://get.docker.com/ | bash

$ # 如果上面的不行，执行下面两句
$ curl -fsSL https://get.docker.com -o get-docker.sh
$ sudo sh get-docker.sh

$ # 安装成功执行下面语句，如果有类似回显，说明安装成功
$ docker --version
Docker version 18.06.1-ce, build e68fc7a
```


## 手动安装

 
联网安装：


> 注意：ppc64le和s390x架构下，只支持Xenial以上的Ubuntu


卸载旧版本

卸载旧版本主要是为了清理安装环境，如果用户没有安装过 docker ，不用考虑该章节。

旧版本的docker叫做docker或者docker-engine ，如果有安装，先卸载其以及其依赖，新版本的docker叫做docker-ce。  
/var/lib/docker/目录下的镜像文件，容器，卷和网络将会被保留，不会被删除。

```bash
sudo apt-get remove docker docker-engine docker.io containerd runc
```

安装

```bash
# 更新apt包索引
sudo apt-get update

# 安装依赖
sudo apt-get install apt-transport-https ca-certificates curl software-properties-common

# 添加官方的GPG key
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo apt-key add -

# 设置稳定版源

# x86_64 / amd64架构
sudo add-apt-repository \
   "deb [arch=amd64] https://download.docker.com/linux/ubuntu \
   $(lsb_release -cs) \
   stable"


# armhf架构
sudo add-apt-repository \
   "deb [arch=armhf] https://download.docker.com/linux/ubuntu \
   $(lsb_release -cs) \
   stable"


#  arm64架构
sudo add-apt-repository \
   "deb [arch=arm64] https://download.docker.com/linux/ubuntu \
   $(lsb_release -cs) \
   stable"


#  IBM Power (ppc64le)
sudo add-apt-repository \
   "deb [arch=ppc64el] https://download.docker.com/linux/ubuntu \
   $(lsb_release -cs) \
   stable"


#  IBM Z (s390x)
sudo add-apt-repository \
   "deb [arch=s390x] https://download.docker.com/linux/ubuntu \
   $(lsb_release -cs) \
   stable"

# 更新apt包索引
sudo apt-get update

# 安装最新版本的docker CE
sudo apt-get install docker-ce

# # 安装特定版本的 docker
# 
# # 查询仓库中 docker 的版本列表
# sudo apt-cache madison docker-ce
# 
# #安装指定版本的docker
# sudo apt-get install docker-ce=18.03.0~ce-0~ubuntu

```



从DEB包安装

这个官方地址列出了所有Debian版本的docker，选择一个进行下载。  
[https://download.docker.com/linux/ubuntu/dists/](https://download.docker.com/linux/ubuntu/dists/)  
在浏览器打开上述地址，导航至pool/stable/，根据自己的架构amd64, armhf, ppc64el, or s390x下载.deb文件  
比如我下载的文件叫做（在当前目录）  
docker-ce_18.06.1_ce_3-0_ubuntu_amd64.deb  


```bash
# 在文件所在目录进行安装:
sudo dpkg -i docker-ce_18.06.1_ce_3-0_ubuntu_amd64.deb
```

## 参考  
[1] 史上最全（全平台）docker安装方法, https://zhuanlan.zhihu.com/p/54147784  