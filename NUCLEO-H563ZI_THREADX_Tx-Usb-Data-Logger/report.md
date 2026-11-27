# NUCLEO-H563ZI ThreadX Tx-Usb-Data-Logger 复盘

本工程从 [`Tx-Data-Logger`](../NUCLEO-H563ZI_THREADX_Tx-Data-Logger/report.md)
（提交 `c73626d`）派生，目标是对应 [Application Note](../Application_note.md)
路线 2 的 **P3：USB 导出（MSC）+ 命令通道（CDC）**。

采集、FileX/LevelX、SD（SPI）、RTC 时间戳、可靠性层与自检脚本原样继承，
设计与踩坑记录见上游的 `report.md`，此处只记录 P3 自身的改动与实测。

## 1. 基线迁移

派生时只做了三件事，不碰任何业务代码：

1. `cp -a` 完整复制上游目录（含 `.vscode`、`.gitignore`、`scripts`）
2. `Makefile` 的 `TARGET` 与 `.vscode/launch.json` 的 `executable` 改为新工程名
3. README 标题与"与上游的关系"章节改写

**验收方式**：新工程在未改任何代码的情况下编译，产物必须与上游逐字节一致。

```
$ arm-none-eabi-size Tx-Usb-Data-Logger.elf
   text    data     bss     dec     hex
 209316    2420  155464  367200   59a60

$ md5sum */NUCLEO-H563ZI_THREADX_Tx-*-Data-Logger.bin
da8d1f28b7c3f6837b6a2124068df1d0  ...Tx-Usb-Data-Logger.bin
da8d1f28b7c3f6837b6a2124068df1d0  ...Tx-Data-Logger.bin
```

二进制相同即证明"复制没有引入差异"，后续任何功能退化都能归因到新代码。

**板上复验**：在新目录执行 `./scripts/selfcheck.sh all`（会 `make clean` 后重建、
重新烧写，再跑八组串口断言），退出码 0。脚本用 `set -e`，且 `serial_check.py`
在任一断言失败时 `return 1`，因此退出码 0 等价于八组全部 PASS：

```
basic    fs    acq    mch    dma    rtc    watchdog    fault
 PASS   PASS   PASS   PASS   PASS   PASS     PASS      PASS
```

这一步确认了 OpenOCD 烧写路径、`/dev/ttyACM0`（ST-LINK VCP → USART3）与新目录
的相对路径都正常，后续 USBX 出问题时有干净的对照点。

## 2. P3-1：CDC-ACM 命令通道

### 2.1 为什么先做 CDC 而不是 MSC

MSC 要回答"PC 挂载了这个卷，FileX 认为它还独占着它，谁说了算"。这个问题没有
便宜的答案（只读导出、卸载/重挂状态机、双区快照，见
[Application Note](../Application_note.md)）。CDC 没有这个问题：它就是一根串口，
复用现有 `console_dispatch()`，一行命令解析都不用改。

而 CDC 已经解决了 P3 的实际痛点：导出文件（`dump`）和远程敲命令。所以先做它。

### 2.2 手写描述符，不要 ST 的 builder

ST 的 `ux_device_descriptors.c` 在不同样例里是 766–1021 行，因为它要在运行时
按 `UserClassInstance[]` 拼装任意类组合、自动分配接口号和端点号、插 IAD。本项目
只有一个类、两个接口、四个端点，写成 85 字节的静态数组：

- 可以直接对着 USB 2.0 规范和 CDC 1.10 逐字段核对
- 编译期就是常量，进 Flash 不占 RAM
- 端点号写死（0x81/0x02/0x83），和 `ux_stm32_config.h` 的 8 端点预算一眼对得上

**单功能 CDC 不需要 IAD**：设备描述符里 `bDeviceClass=0x02` 已经告诉主机这是个
CDC 设备，主机知道要去接口 0 找通信类、接口 1 找数据类。只有复合设备（CDC+CDC、
CDC+HID）才必须用 IAD 把多个接口绑成一个功能。

### 2.3 两个编译期坑

**1. `ux_stm32_config.h` 必须带 HAL 头。** STM32 DCD 端口（`ux_dcd_stm32*.c`）
直接使用 `PCD_HandleTypeDef` 和 `HAL_PCD_EP_Open` 等，但它自己不 include HAL。
ST 样例是在 `ux_stm32_config.h` 里 `#include "stm32h5xx_hal.h"` 解决。照抄样例时
把这个 include 简化掉，会得到一堆 `unknown type name 'PCD_HandleTypeDef'`——
错误指向 DCD 文件，根因却在自己写的配置头里。

**2. `USB_CfgTypeDef` 的字段随内核变化。** `use_dedicated_ep1` 只在
`USB_OTG_FS/HS` 下存在；`USB_DRD_FS` 用的是 `bulk_doublebuffer_enable` 和
`iso_singlebuffer_enable`。从 OTG 板（如 H573、F4）的样例抄 PCD 初始化会编译
不过。

### 2.4 真正需要设计的地方：让 USB 不触发看门狗

本工程的看门狗检查**每个**心跳 id：任意线程 1.5 s 不打心跳就停止喂 IWDG，
4 s 后复位。而 USBX 的 `ux_device_class_cdc_acm_read()` 默认阻塞长达
`UX_NON_CONTROL_TRANSFER_TIMEOUT`（50 s）。

于是"主机插着但没敲命令"这种完全正常的状态，会被判定成"usb_rx 线程卡死"，
进而复位板子——每 4 秒一次。

解决办法是 `SET_READ_TIMEOUT` / `SET_WRITE_TIMEOUT` 两个 ioctl（`ux_device_class_cdc_acm_ioctl`），
在激活回调里把超时压到 **10 tick / 20 tick**（100 ms / 200 ms）。注意参数单位是
**tick** 而不是毫秒：ioctl 直接把值写进 `ux_slave_transfer_request_timeout`，
而 DCD 别处是用 `UX_MS_TO_TICK()` 换算后写入的。

超时后 `read()` 返回非 `UX_SUCCESS`，循环下一次迭代就打了心跳。链路空闲与链路
故障因此在看门狗眼里都是"活着"，真正的挂死仍然会暴露（线程不再返回）。

同理，`usb_tx` 线程用 `tx_event_flags_get(..., 100ms)` 而不是 `TX_WAIT_FOREVER`。

### 2.5 输入/输出的接线方式

输入：CDC 收到的字节进**自己的**环形缓冲，`console` 线程每轮把 UART 环和 USB 环
都抽干，各自维护一个行缓冲，最后都调用 `console_dispatch()`。放在同一个线程
里是有意的——`console_dispatch()` 及其下游（`app_log_printf`、文件系统命令）
并不是可重入的，两个线程同时进命令处理会出问题。

输出：`app_log_drain()` 里多一句 `app_usb_write()`。它只往环形缓冲拷字节，不做
USB 传输，所以 logger 线程（优先级 20，最低）不会因为主机慢而被拖住。

### 2.6 实测

```
$ # 未接用户 USB-C 时
USB ready=1 connected=0 rx=0 tx=0 rx_drop=0 tx_drop=0
STACK ... usb_rx=984/1536 usb_tx=708/1536
```

`usb_rx` 第一次跑完整 USBX 初始化（`ux_system_initialize` →
`ux_device_stack_initialize` → 类注册 → DCD 初始化）用了 984 B，1536 B 只剩
552 B 余量，据此把栈提到 2048 B。

链接 USBX 后其余八组自检全部仍 PASS：

```
RESULT basic: PASS    RESULT fs: PASS     RESULT acq: PASS    RESULT mch: PASS
RESULT dma: PASS      RESULT rtc: PASS    RESULT watchdog: PASS  RESULT fault: PASS
SKIP usb: no CDC port found; connect the user USB-C to the host
```

看门狗那组尤其关键：它证明新加的两个心跳不会把自己判成卡死。

### 2.7 插线之后：一个固件缺陷与一个方法错误

第一次插线没有任何设备出现。USB 控制器侧的读数一开始就自相矛盾：`BCDR.DPPU`
已置位（D+ 上拉开）、`CNTR` 显示核心已使能、`ISTR` 读到 0。当时我由
`ISTR=0` 推出"主机从未复位过设备"，**这个推理是错的**：`ISTR` 的标志位会被中断
服务程序清掉，读到 0 只能说明"此刻没有待处理事件"。判断"有没有收到过复位"必须
看累计量，于是补了两个观测点：中断计数和 USBX 的设备状态机。

第一个真正的固件缺陷在 Type-C 端接。用户口是 Type-C 母座 + TCPP01 保护器件，
CC1/CC2 接 UCPD1（PB13/PB14）而不是固定电阻。主机必须先看到 sink 端接才会加
VBUS，所以缺 Rd 时端口对主机完全不可见。补上 `app_usb_ucpd_init()` 之后又踩到
顺序问题：

```c
LL_UCPD_Enable(UCPD1);        /* 必须先置 UCPDEN */
HAL_PWREx_DisableUCPDDeadBattery();
LL_UCPD_SetSNKRole(UCPD1);    /* 这两句只在 UCPDEN=1 之后才锁存 */
LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);
```

把角色写在使能之前，`UCPD1->CR` 读回就是 `0x00000000`——表现和"没插线"一模一样，
极容易误判。修正后 `CR=0x00000e00`，CC 也开始报告电压档位。

### 2.8 结论：用户口的数据通路上不来

修正端接后，寄存器给出的证据已经齐全：

```
USB DEV state=0 dcd=1 pcd=1 irq=0
USB REG fnr=0x00008000   -> RXDP=1, RXDM=0  （FS 空闲态正确）
USB CC  cc1=0 cc2=1      -> CC2 上是 Rp 默认档，主机已附着
```

`irq=0` 是自开机以来的累计中断数，它为零意味着主机一个 RESET、一个 SOF 都没发过。
两项旁证排除了固件与线材：

| 对照 | 结果 |
| --- | --- |
| 同一根线插 ST-LINK 口 | 正常枚举 `0483:374e`；换另一根线同样正常，说明两根线都能传数据 |
| 烧回曾在本板枚举成功的 ST HID 固件 | 同样不枚举 |

所以剩余故障点在**用户口的 D+/D− 通路**（TCPP01 或连接器/焊盘），与 USBX 代码
无关。这与本工程已确认的"外部模拟输入通路损坏"属于同一类故障，且物理位置相邻。
P3-1 的端到端验证只能等新板子。

留下的诊断命令（`usb`、`usb regs`、`usb cc`）不依赖板子好坏，换板后仍可直接用。

## 3. P3-2：MSC 只读数据盘

阻塞项：本板用户口数据通路损坏，CDC 尚未在硬件上验证通过；MSC 需要先有可用的
USB 链路。

## 4. P3-3：MSC 读写与介质接管

（待实现）

## 5. 实测数据

### 5.1 8 小时长跑（首次通过）

条件：4 通道 / 10 kHz，内部通道，SD 卡上 128 MB 卷，轮转 4 MB x 15。

```
文件         行数      时间范围                    bad_ts
LOG0000     5,088    00:02:43 -> 00:06:58          0
LOG0001    64,054    00:00:00 -> 00:53:23          0
LOG0002    63,137    00:53:23 -> 01:46:00          0
LOG0003    62,601    01:46:00 -> 02:38:10          0
LOG0004    61,830    02:38:10 -> 03:29:41          0
LOG0005    61,680    03:29:41 -> 04:21:05          0
LOG0006    61,680    04:21:06 -> 05:12:30          0
LOG0007    61,680    05:12:30 -> 06:03:54          0
LOG0008    61,680    ...    -> 06:55:18            0
LOG0009    61,680    06:55:18 -> 07:46:42          0
LOG0010    11,180    07:46:42 -> 07:56:01          0
```

- 共 **571,202 个窗口** = 1,142,404,000 个样本，**38.8 MB** 落盘
- 每个文件都满足 `samples == lines x 500 x 4`（精确等式，差 1 即失败）
- **时间戳跨文件严格衔接**：每个文件的 `last` 等于下一个文件的 `first`，
  这是"轮转没有丢样"的直接证据
- 全程 `drop=0 dma_ovr=0 qdrop=0 log_drop=0`

SOAK.LOG 的逐分钟记录（80785 字节，约 480 条）：

| 时刻 | elapsed_s | windows | drop | dma_ovr | qdrop | 栈 monitor/console/writer |
| --- | --- | --- | --- | --- | --- | --- |
| 4 分钟 | 240 | 4,799 | 0 | 0 | 0 | 1008 / 2444 / 16xx |
| 7.97 小时 | 28,680 | 568,801 | 0 | 0 | 0 | 1008 / 2476 / 1776 |

**没有内存泄漏**：monitor 栈 8 小时完全不变（1008），console 只涨 32 字节
（2444 -> 2476），属于首次触达更深调用路径后的一次性增长，不是持续攀升。

### 5.2 这次运行暴露的一个设计事实：idx 在续跑时会重零

`idx` 是"本次启动内的窗口序号"（`acq_windows_done`），`app_acq_start()` 会把它
清零。所以 8 小时跑下来实际是**两段**：

| 段 | 文件 | idx 范围 |
| --- | --- | --- |
| 换电源前 | LOG0000 | 0..5087 |
| 换电源后（连续 7.9 h） | LOG0001..LOG0010 | 0..571201 |

跨段的连续性不能靠 `idx` 判断，要看 `epoch` 时间戳。段内连续性由每段的
`samples == lines x 500 x nch` 保证。

### 5.3 如何事后校验一个跑完的目录

```sh
sd init / acq target sd
acq check LOG0000.CSV      # 指定文件，不依赖"当前正在写"的那个
sd cat SOAK.LOG 80300      # 带偏移读，越过 511 字节的输出上限
```

`acq check <file>` 与 `sd cat <file> [offset]` 都是这次补的：跑完之后没有任何
文件是"当前文件"，而 80 KB 的 SOAK.LOG 也不是一次 `sd cat` 能看完的。

### 5.4 环境注记

本板无 VBAT 电池，换电源后 RTC 归零，所以 CSV 里的绝对时间从 2000-01-01 起算。
`epoch` 的**单调性和相对间隔**仍然可信（`bad_ts=0` 就是在验这个），但绝对时间
不可用。真机部署需要装纽扣电池。
