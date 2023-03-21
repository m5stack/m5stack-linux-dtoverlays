# CM4Stack_QtDemo
Qt demo for CM4STACK

### Dependency

```bash
sudo apt update
sudo apt install qtbase5-dev qt5-qmake qtbase5-dev-tools qml
sudo apt install build-essential cmake 
```

### Get code

```bash
git clone https://github.com/Forairaaaaa/CM4Stack_QtDemo.git
cd CM4Stack_QtDemo
```

### Build

```bash
mkdir build && cd build
cmake .. && make
```

### Run

```bash
./cm4QtDemo
```

### Display on specified framebuffer

```bash
# export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb1
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb$(cat /proc/fb | grep fb_st7789v | awk '{print $1}')
./cm4QtDemo
```

Demo will display on `/dev/fb1`, learn more:  https://doc.qt.io/qt-6/embedded-linux.html

You can unset this config:

```bash
unset QT_QPA_PLATFORM
```

