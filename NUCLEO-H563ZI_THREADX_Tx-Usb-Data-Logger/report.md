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

（待实现）

## 3. P3-2：MSC 只读数据盘

（待实现）

## 4. P3-3：MSC 读写与介质接管

（待实现）

## 5. 实测数据

（待填）
