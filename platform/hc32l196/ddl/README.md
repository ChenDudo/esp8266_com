# ddl —— HC32L19x 官方驱动库（子集）

## 来源

从小华半导体（Xiaohua Semiconductor / XHSC）的 **`HC32L19x_DDL_Rev1.3.0`** 拷贝而来：

| 目录 | 内容 |
| --- | --- |
| `driver/inc`、`driver/src` | 官方外设驱动（32 个头文件 + 32 个源文件，**全部拷了**，方便以后加 ADC/SPI/I2C…） |
| `mcu/common` | 器件头文件 `hc32l19x.h`、`base_types.h`、`system_hc32l19x.*`、`interrupts_hc32l19x.*`、`board_stkhc32l19x.h` |

启动文件 `startup_hc32l19x.s` 与器件选择头 `ddl_device.h` 放在
`../example/MDK/` 和 `../example/source/` 下（这是官方例程的组织方式）。

## 许可证

文件头声明为 **XHSC BSD 3-Clause**（`opensource.org/licenses/BSD-3-Clause`）。
拷贝时**保留了原始版权头、未做任何修改**，符合该许可证的要求。

## 为什么放进仓库

这样 Keil 工程是**自包含**的：换机器、别人克隆下来都能直接编译下载，
不用再去对 DDL 的路径。仓库里唯一要维护的"自己的代码"是 `host/` 和
`platform/hc32l196/example/source/main.c`。

## 升级方式

以后换新版 DDL：把本目录的 `driver/` 与 `mcu/common/` 整体覆盖，
然后确认 `../example/source/ddl_device.h` 里的两个宏仍然是：

```c
#define DDL_MCU_SERIES    DDL_DEVICE_SERIES_HC32L19X
#define DDL_MCU_PACKAGE   DDL_DEVICE_PACKAGE_HC_K
```

> 注意：Keil 工程只编译 `ddl_driver` 分组里列出的 4 个源文件
> （`ddl.c`、`sysctrl.c`、`gpio.c`、`uart.c`）。本目录里其它的 `.c`
> 不会被编译，需要时手工加进工程。
