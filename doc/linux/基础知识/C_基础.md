# C 编程基础
关于编程的基础思想可以查看 [编程基础](./编程基础.md)

## 第一个 C 程序 Hello, World!

```c 
// hello.c
#include <stdio.h>
 
int main()
{
    /* 我的第一个 C 程序 */
    printf("Hello, World! \n");
 
    return 0;
}
```
```bash
#编译
gcc hello.c -o hello
# 执行，查看输出结果！
./hello
#Hello, World!
```

这是一个 c 的 hello world！经由编译器编译执行后，你就能得到控制台的一个 "Hello, World!" 的输出。   
关于编程平台，我比较建议使用 linux 进行编程。linux 从诞生就是为编程和控制的，它还自带编译器。但是在 windows 平台上单单的编译器安装就是一个比较大的门槛。
刚开始也可以使用网上的学习平台，比如《[菜鸟教程](https://www.runoob.com/cprogramming/c-tutorial.html)》,可以直接在网页上运行程序，也算是一个比较好的学习网站。



## 参考
[1] [菜鸟教程](https://www.runoob.com/cprogramming/c-tutorial.html) : https://www.runoob.com/cprogramming/c-tutorial.html
[2] 《The C programming language》
