# ax620 yolov5s 训练部署教程

unitv3 使用爱芯 ax620u 作为主控芯片，本文提供一个如何制作 AI 模型的数据集，然后上传至在线训练平台进行训练，最后部署到设备上的教程。

## 1. Yolov5简介
&emsp;&emsp;YOLOv5 模型是 Ultralytics 公司于 2020 年 6 月 9 日公开发布的。YOLOv5 模型是基于 YOLOv3 模型基础上改进而来的，有 YOLOv5s、YOLOv5m、YOLOv5l、YOLOv5x 四个模型。YOLOv5 相比YOLOv4 而言，在检测平均精度降低不多的基础上，具有均值权重文件更小，训练时间和推理速度更短的特点。YOLOv5 的网络结构分为输入端BackboneNeck、Head 四个部分。  
&emsp;&emsp;本教程针对目标检测算法 yolov5 的训练和部署到 unitv3(ax620u) 进行说明。
![](./img/h7k633w7lmbbefyc8luw.png)


## 2. 准备数据集

### 2.1 准备图片数据
本教程以垃圾分类检测为例，数据集的下载链接为: [https://github.com/Abandon-ht/coco_rubbish_dataset/archive/refs/heads/main.zip](https://github.com/Abandon-ht/coco_rubbish_dataset/archive/refs/heads/main.zip)  
这里使用已经准备好的数据集，下一步根据参考的数据集制作自己的数据集。  

### 2.2 标注工具的安装与使用

labelimg 是一个可视化的图像标定工具。它是用Python编写的，并将Qt用于其图形界面。批注以PASCAL VOC格式（ImageNet使用的格式）另存为XML文件。此外，它还支持YOLO格式。Faster R-CNN，YOLO，SSD等目标检测网络所需要的数据集，均需要借此工具标定图像中的目标。

#### 安装
首先确保本机已经安装了 python3 ，同时能够在终端中使用 python3 。
```bash
pip install LabelIMG
```

#### 使用
终端运行 labelImg 即可。
```bash
labelImg
```

#### 使用 labelImg 进行数据集标注
&emsp;&emsp;labelImg 的标注模式分为 VOC 和 YOLO 两种，两种模式下生成的标注文件分别为.xml文件和.txt文件，因此在进行标注前需要优先选择好标注的模式。在 yolov5s 的训练中，我们选择 YOLO 数据集。
![](./img/hjx3xg16ileqvudn5dwk.png)

labelImg 左侧菜单栏中按钮功能说明如下。

|按钮 |	功能|
|---|---|
|Open |	打开单个图像文件|
|Open Dir |	打开文件夹多幅图像 |
|Change Save Dir |	标注后图像数据存储的路径 |
|Next Image |	切换到下一张图像|
|Prev Image |	切换到上一张图像|
|Verify Image |	校验图像|
|Save |	保存图像|
|Create RectBox |	画一个标注框|
|Duplicate RectBox |	重复标注框|
|Delete RectBox |	删除标注框|
|Zoom In |	放大图像|
|Zoom Out |	缩小图像|
|Fit Window |	图像适应窗口|
|Fit Width |	图像适应宽度|

&emsp;&emsp;在 labelImg 中，标注的快捷键为 w ，标注后用鼠标拖动锚框进行框选，框选完毕后会弹出类别选择框，如果是当前已有类则直接选择即可，若需要新加类别则在输入框中输入类别标签并确定。

( YOLO 标注模式下)标注完成后数据集整体结构如下图所示。
![](./img/r9zeslq0n2pyq2gxthkr.png)

#### 标签文件解释
&emsp;&emsp;使用 labelImg 标注产生的结果如下。
![](./img/lpetswpu8zwx1rq25usc.png)

&emsp;&emsp;使用 labelImg 以 VOC 模式进行标注产生的标注文件如下，与 YOLO 标注模式产生的类别标签不同，.xml 标注文件不需要将类别文件单独进行存放，因为其中已经包含了标注锚框的位置信息和类别信息。
![](./img/bsj88946um2shek011pw.png)

&emsp;&emsp;使用 labelImg 以 YOLO 模式进行标注产生的标注文件如下，classes.txt 文件中存放的是类别标签，标签文件中以类别+锚框的位置坐标信息进行保存，文件中有几行则说明对应的标注图像中有几个锚框。
![](./img/7m63g6eqaq0b54eqb76i.png)

#### labelImg 操作快捷键汇总

labelImg 完整的快捷键列表如下。

|快捷键| 	功能| 	快捷键| 	功能|
|---|---|---|---|
|Ctrl + Q |	退出软件 |	Ctrl + + |	放大|
|Ctrl + O |	打开单个图像文件 |	Ctrl + - |	缩小|
|Ctrl + U |	Open Dir |	Ctrl + = |	原始大小|
|Ctrl + R |	Change Save Dir |	Ctrl + F |	图像适应窗口|
|Ctrl + S |	保存图像 |	Ctrl + E |	编辑标签|
|Ctrl + L |	线框颜色 |	W | 	画框|
|Ctrl + J |	移动编辑标注框 |	Delete |	删除框|
|Ctrl + D |	复制框 |	Ctrl + shift + F |	图像适应宽度|
|Ctrl + H |	隐藏所有的框 |	D| 	Next Image|
|Ctrl + A |	显示所有的框 |	A |	Prev Image|
|space |	标记当前图片已标记 |	Ctrl + shift + O |	打开的文件夹只显示.xml文件|


#### VOC 数据集目录树
```bash
dataset.zip
.
├── images # 训练图片目录，每个类别不少于 35 张图片
│   ├── IMG_20210410_001401.jpg
│   └── IMG_20210410_001403.jpg
├── labels # 训练标签目录
│   ├── IMG_20210410_001401.txt
│   └── IMG_20210410_001403.txt
├── train2017.txt # 训练数据集
└── val2017.txt # 验证数据集
```
有了训练数据集后，使用 zip 压缩软件进行打包。


## 3. 训练


## 3.1 在线训练  
打开在线训练网站 []() ,填写好邮箱地址并上传数据集，等待训练完成。



## 3.2 本地训练  

参考：yolov5 仓库中的 [AX620_README](https://github.com/m5stack/yolov5/blob/ax_620u/AX620_README.md)

### 开始

首先创建工作目录,以下所有操作均在此目录内。

右键打开终端,执行以下命令:
```bash
cd ~
mkdir workspace && cd workspace
export TRAIN_HOME=`pwd`
```

![](系统环境配置情况或推荐环境配置)

### 一、准备数据集

如何制作目标检测数据集请参考第二章的内容,本教程用标注好的“垃圾检测“数据集对整个流程进行讲解。该数据集可以通过以下三种方式获取:

1.直接下载

Github: https://github.com/Abandon-ht/coco_rubbish_dataset/archive/refs/heads/main.zip

将下载的数据集解压到 datasets 文件夹,并重命名为 rubbish

2.拉取数据集的仓库

```bash
mkdir $TRAIN_HOME/datasets && cd $TRAIN_HOME/datasets
git clone https://github.com/Abandon-ht/coco_rubbish_dataset.git rubbish
```

3.终端命令下载

```bash
mkdir $TRAIN_HOME/datasets && cd $TRAIN_HOME/datasets
wget https://github.com/Abandon-ht/coco_rubbish_dataset/archive/refs/heads/main.zip
unzip coco_rubbish_dataset-main.zip
mv coco_rubbish_dataset-main rubbish
```

三种方法都可以得到以下 2 个文件夹和 3 个文件

![](./images/002.png)


### 二、获取 yolov5 源码

在 workspace 目录下(注意不是在 datasets 目录下),拉取 yolov5 源码的仓库

```bash
cd $TRAIN_HOME
git clone https://github.com/m5stack/yolov5.git  # clone
cd yolov5
pip install -r requirements.txt  # install
```

yolov5 目录如图所示:

![](./images/004.png)


### 三、训练 yolov5 模型

进入 yolov5 的工作目录, 获取 yolov5s 模型权重
```bash
cd $TRAIN_HOME/yolov5
mkdir weights
wget -P weights https://github.com/ultralytics/yolov5/releases/download/v6.2/yolov5s.pt
```

复制一份 coco.yaml,并重命名为 rubbish.yaml

```bash
cd $TRAIN_HOME/yolov5
cp data/coco.yaml data/rubbish.yaml
```

根据图片修改垃圾分类数据集的路径和类别名字

```bash
# Train/val/test sets as 1) dir: path/to/imgs, 2) file: path/to/imgs.txt, or 3) list: [path/to/imgs1, path/to/imgs2, ..]
path: ../datasets/rubbish  # dataset root dir
train: train2017.txt  # train images (relative to 'path') 118287 images
val: val2017.txt  # val images (relative to 'path') 5000 images
# test: test-dev2017.txt  # 20288 of 40670 images, submit to https://competitions.codalab.org/competitions/20794

# Classes
names:
  0: battery
  1: pills
  2: bananas
  3: apples
  4: cans
  5: bottles
  6: ceram
  7: green vegatable
  8: broccoli
  9: boxes
  10: glass
  11: cigarette
  12: orange
  13: eggshell
  14: spitball
  15: tile
```

![](./images/005.png)

修改完成以后,用以下命令开始训练 yolov5s 模型

```bash
cd $TRAIN_HOME/yolov5
python train.py --data data/rubbish.yaml --cfg models/yolov5s.yaml --weights weights/yolov5s.pt --batch-size -1 --epoch 20
```

![](./images/006.png)

数据集加载成功,开始训练模型。如果没有加载成功,请检查数据集路径

![](./images/007.png)

训练完成后,可以在 ./runs/train/exp/ 文件夹下查看训练的日志

> 注意：每次训练都会在 ./runs/train 目录下生成一个新的 exp 目录，按自然数递增，多次训练时注意产生的模型路径。

PR_curve.png 为 mAP_0.5 曲线

![](./images/PR_curve.png)

results.png 为全部曲线

![](./images/results.png)

### 四、模型预测和导出

可以使用以下命令预测图片,注意需要将图片和模型的路径修改为自己的路径

```bash
cd $TRAIN_HOME/yolov5
python detect.py --source ../datasets/rubbish/images/IMG_20210311_213716.jpg --weights ./runs/train/exp/weights/best.pt
```

![](./images/008.png)

可以在 runs/detect/exp 目录下。看到预测的图片

![](./images/009.jpg)

使用以下命令导出 onnx 模型,注意加上 opset=11 和 ax620 这两个参数

<!-- ![](./images/010.png) -->

```bash
cd $TRAIN_HOME/yolov5
python export.py --include onnx --opset 11 --weights ./runs/train/exp/weights/best.pt ax620
```

导出的 onnx 模型在 runs/train/exp/weights 目录下

![](./images/010a.png)

在浏览器地址栏中输入 [netron.app](netron.app) ,把上一步导出的 best.onnx 文件拖进去,查看 yolov5 的模型结构。

模型的输入名字为:images

![](./images/011a.png)

可以看到输出如下图所示，其中黄色框的数字应该等于你的（模型训练的类别+5）*3，如果上文下载的模型，则是 yolov5 16 个类的目标检测数据集训练的模型，63=(16+5)*3

![](./images/012a.png)

使用以下命令导出模型的 anchors 参数
```bash
python export_anchors.py runs/train/exp/weights/best.pt
```
![](./img/asxsadasdsafadsas.png)


### 五、打包训练图片

进入数据集的图片目录,使用以下命令打包图片为 rubbish_1000.tar ,注意文件的扩展名是 .tar 。图片不需要打包太多，> 35 张图即可。图片过多在下一步的量化过程会比较慢。

```bash
cd $TRAIN_HOME/yolov5/datasets/rubbish/images/
tar -cvf ../rubbish_1000.tar *.jpg
```

<!-- ![](./images/030.png) -->

<!-- ![](./images/031.png) -->

### 六、搭建模型转换环境

onnx 模型需要转换为 joint 模型才能在 m3axpi 运行,所以需要使用 pulsar 模型转换工具。注意 pb、tflite、weights、paddle 等模型,需要先转换为 onnx 模型才能使用 pulsar 模型转换工具

使用以下命令拉取带有模型转换工具的容器,没有安装docker请自行安装

```bash
docker pull sipeed/pulsar:0.6.1.20
```

<!-- ![](./images/032.png) -->

使用以下命令进入容器,如果需要保留该容器,请删除 --rm 这个参数。注意一定要设置共享内存,将 workspace 工作目录挂载到容器的 data 目录

```bash
docker run -it --net host --rm --shm-size 16g -v cd $TRAIN_HOME:/data sipeed/pulsar
```

<!-- ![](./images/033.png) -->

> 尽量不要使用 Nvidia GPU 环境，不然你会遇到，pytorch 版本过低，调用 gpu 出错，依旧使用 cpu 单线程进行转换等问题。

<!-- ![](./images/034.png) -->

在工作目录 data 下创建 config 和 onnx 文件夹。

```bash
mkdir config onnx
cp yolov5/runs/train/exp/weights/best.onnx onnx/
```
![](./images/034a.png)

在 config 下创建一份命名为 yolov5s_rubbish.prototxt 的文件,复制以下内容到文件,注意修改文件中 rubbish_1000.tar 的路径
```
# my_config.prototxt

# 基本配置参数:输入输出
input_type: INPUT_TYPE_ONNX

output_type: OUTPUT_TYPE_JOINT

# 选择硬件平台
target_hardware: TARGET_HARDWARE_AX620

# CPU 后端选择,默认采用 AXE
cpu_backend_settings {
    onnx_setting {
        mode: DISABLED
    }
    axe_setting {
        mode: ENABLED
        axe_param {
            optimize_slim_model: true
        }
    }
}

# input
src_input_tensors {
    color_space: TENSOR_COLOR_SPACE_RGB
}

dst_input_tensors {
    color_space: TENSOR_COLOR_SPACE_RGB
}

# neuwizard 工具的配置参数
neuwizard_conf {
    operator_conf {
        input_conf_items {
            attributes {
                input_modifications {
                    affine_preprocess {
                        slope: 1
                        slope_divisor: 255
                        bias: 0
                    }
                }
                # input_modifications {
                #     input_normalization {
                #         mean: [0,0,0]
                #         std: [0.5,0.5,0.5]
                #     }
                # }
            }
        }
    }
    dataset_conf_calibration {
        path: "/data/dataset/rubbish_1000.tar" # 数据集图片的 tar 包,用于编译过程中对模型校准
        type: DATASET_TYPE_TAR         # 数据集类型:tar 包
        size: 256                      # 编译过程中校准所需要的实际数据个数为 256
    }
    dataset_conf_error_measurement {
        path: "/data/dataset/rubbish_1000.tar" # 用于编译过程中对分
        type: DATASET_TYPE_TAR
        size: 4                        # 对分过程所需实际数据个数为 4
        batch_size: 1
    }

}

dst_output_tensors {
    tensor_layout:NHWC
}

# pulsar compiler 的配置参数
pulsar_conf {
    ax620_virtual_npu: AX620_VIRTUAL_NPU_MODE_111
    batch_size: 1
    debug : false
}
```
![](./images/035.png)

移动导出的模型文件 best.onnx 到 onnx 目录,使用以下命令进行模型转换:(注意修改模型文件的名字改为自己的模型名字)

```bash
pulsar build --input onnx/best.onnx --output yolov5s_rubbish.joint --config config/yolov5s_rubbish.prototxt --output_config yolov5s_rubbish.prototxt
```
开始转换

![](./images/036.png)

转换时间较长,请耐心等待

![](./images/037.png)

转换完成

![](./images/038.png)

可以在工作目录下找到转换后的模型 yolov5s_rubbish.joint

![](./images/039.png)

### 七、部署

<!-- 
请确认经过了训练后拥有以下文件，最起码得得到 `yolov5s_rubbish.joint` 喔。

![](./yolov5s_result/images/01.png)

打开 VSCODE 安装 REMOTE SSH 远程开发插件，不了解的看这篇 远程开发初探 - [VSCode Remote Development](https://zhuanlan.zhihu.com/p/82568294)

![](./yolov5s_result/images/02.png)

这里我走的是板子 USB RNDIS 网卡的 IP 地址 root@192.168.233.1 ，不了解的看这篇 [Maix-III AXera-Pi 系统基础使用 - Sipeed Wiki](https://wiki.sipeed.com/hardware/zh/maixIII/ax-pi/basic_usage.html#%E5%9F%BA%E4%BA%8E-ip-%2B-ssh-%E7%99%BB%E5%BD%95)

![](./yolov5s_result/images/03.png)

等待登陆成功，如果没有连上请 `ping 192.168.233.1` 并查阅文档解决网络链接问题。

![](./yolov5s_result/images/04.png)

登陆成功，输入默认密码 `root` 即可远程登陆板上系统，就是登陆一台云服务器操作。

![](./yolov5s_result/images/05.png)

接着，切换一下工作区，这里选根文件系统方便看到系统所有文件。

![](./yolov5s_result/images/06.png)

现在我们把文件都拖拽进板子的 home 目录下，表示这是我们的工作区，如下图。

![](./yolov5s_result/images/07.png)

现在我们有两种部署方式，第一种是只验证模型推理一张图片结果，第二种是部署到板子上进行摄像头或网络的实时推理。

## 1. ax-sample

项目主页 https://github.com/AXERA-TECH/ax-samples

这是专门给软件算法工程师推理验证单张图片的部署模型验证的工具，可以在板子的 `home/ax-samples` 找到它，这里我为了举例说明省事一些，采用的是本地编译的方式。

![](./yolov5s_result/images/08.png)

将 `ax_yolov5s_rubbish_steps.cc` 拖拽复制到 `home/ax-samples/examples` 下，并在 `home/ax-samples/examples/CMakeLists.txt` 文件的 `ax620 support` 之下依葫芦画瓢添加 `ax_yolov5s_rubbish` 的编译项，意味着待会就会使用 `ax_yolov5s_rubbish_steps.cc` 编译出可以运行的 ax_yolov5s_rubbish 程序。

```c
if (AXERA_TARGET_CHIP MATCHES "ax620a")   # ax620 support
    axera_example (ax_yolov5s_rubbish        ax_yolov5s_rubbish_steps.cc)
    axera_example (ax_classification        ax_classification_steps.cc)
```

然后终端 `cd build` ，并输入 `make` 即可开始编译，需要注意的是这里系统里已经预编译过了，并配置好环境，如果你想交叉编译，请自行配置了解环境搭建后自行配置，在这里只是为了验证和调试。

![](./yolov5s_result/images/09.png)

稍等一下会出现编译通过，程序放在 `examples/ax_yolov5s_rubbish` 等待运行。

![](./yolov5s_result/images/10.png)

编译完成后，我们看过一下改动的地方，例如 `ax_yolov5s_rubbish_steps.cc` 的主要内容如下：

![](./yolov5s_result/images/11.png)

可见它是由 `home/ax-samples/examples/ax_yolov5s_steps.cc` 修改而来，这里的CLASS_NAMES对应自己训练的模型类别名字。CLS_NUM对应自己训练的模型类别数量。使用它就可以推理一张图片给我们的板子。
这里我从网上随便找了一张图像，随便拿个常见的电池垃圾。

![](./yolov5s_result/images/12.png)

把它保存成 `battery.jpg` 放到 `/home/ax-samples/build/examples/battery.jpg` 即可。

![](./yolov5s_result/images/13.png)

接着我们使用这一张图片进行推理，根据 `examples/ax_yolov5s_rubbish -h` 可知使用方法，使用 `examples/ax_yolov5s_rubbish -m /home/yolov5s_rubbish.joint -i examples/battery.jpg -g 640,640 -r 2 `  运行两次推理测试，它会在输出一个 `home/ax-samples/build/yolov5s_out.jpg` 运行结果，方便你分析后处理的推理结果。

![](./yolov5s_result/images/14.png)

这里附图说明一下单张推理的识别结果。

![](./yolov5s_result/images/15.jpg)

可以见到已经能够识别目标，这一切工作顺利，接下来要对其进行实时部署推流。

## 2. ax-pipeline

项目主页 https://github.com/AXERA-TECH/ax-pipeline

该项目基于 AXera-Pi 展示 ISP、图像处理、NPU、编码、显示 等功能模块软件调用方法，方便社区开发者进行快速评估和二次开发自己的多媒体应用，由于 ax-pipeline 已经适配好 yolov5s 模型了，所以只需要配置一下代码就可以使用 python3 代码进行调试。

更多请参考 [Maix-III AXera-Pi 试试 Python 编程 - Sipeed Wiki](https://wiki.sipeed.com/hardware/zh/maixIII/ax-pi/python_api.html)

确认板上的 `/home/yolov5s_rubbish.json` 配置，确认 `/home/yolov5s_rubbish.joint` 模型文件。

```json
{
    "MODEL_TYPE": "MT_DET_YOLOV5",
    "MODEL_PATH": "/home/yolov5s_rubbish.joint",
    "ANCHORS": [
        10, 13, 16, 30, 33, 23, 30, 61, 62, 45, 59, 119, 116, 90, 156, 198, 373, 326
    ],
    "CLASS_NAMES": [
        "battery", "pills", "bananas", "apples", "cans", "bottles", "ceram", "green vegatable",
        "broccoli", "boxes", "glass", "cigarette", "orange", "eggshell", "spitball", "tile"
    ],
    "CLASS_NUM": 16,
    "NMS_THRESHOLD": 0.45,
    "PROB_THRESHOLD": 0.35
}
```

确认板上的 `home/yolov5s_rubbish.py` 代码，摄像头是 gc4653 请使用 `b'-c', b'2'` ，而 os04a10 请用 `b'-c', b'0'`喔。

> 请务必将 ax-pipeline-api 更新到 1.0.7 以上版本再运行以下代码。

```python
import time
from ax import pipeline
pipeline.load([
    'libsample_vin_ivps_joint_vo_sipy.so',
    '-p', '/home/yolov5s_rubbish.json',
    '-c', '2',
])
while pipeline.work():
    time.sleep(0.001)
    tmp = pipeline.result()
    if tmp and tmp['nObjSize']:
        for i in tmp['mObjects']:
            print(i)
        # if tmp['nObjSize'] > 10: # try exit
        #     pipeline.free()
pipeline.free()
```

在终端运行 `python3 /home/yolov5s_rubbish.py`，即可看到效果。

![](./yolov5s_result/images/16.png)

最后附一下 `yolov5s_out.gif` 实拍效果：（ 目前快速退出程序可以用 `CTRL + \` 比 `CTRL + C` 杀得快，零等待 XD ）

![](./yolov5s_result/images/17.gif)

而这里不提及 C/C++ 是相对于零基础的同学来说过于复杂，在系统里已经提供了一些编译好的验证程序，所以 yolov5s 可以直接使用 `/home/bin/sample_vin_ivps_joint_vo -p /home/yolov5s_rubbish.json -c 2` 进行测试。

这与 python 代码的用法和效果是一样的，只是不会输出识别坐标到终端，仅供调试参考，添加业务逻辑请直接修改仓库的 C/C++ 源码，或通过 Python 调试。

想了解更多详细请查阅：[Maix-III AXera-Pi 系统基础使用 - Sipeed Wiki](https://wiki.sipeed.com/hardware/zh/maixIII/ax-pi/basic_usage.html#%E5%86%85%E7%BD%AE%E5%BC%80%E7%AE%B1%E5%BA%94%E7%94%A8)

最后用到的主要文件有如下：

![](./yolov5s_result/images/18.png)


 -->
