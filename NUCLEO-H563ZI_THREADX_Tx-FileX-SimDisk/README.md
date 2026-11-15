# NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk

**在片内 RAM 上模拟一块 NOR 闪存盘，跑真实的 FileX + LevelX 文件系统栈**，
不需要 SD 卡座或外部 Flash。从
[`Tx-Reliability-Monitor`](../NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)
派生，继承日志、看门狗、故障记录、状态台与一键自检；本次新增文件系统层。

对应 [Application Note](../Application_note.md) 路线 2（数据记录仪）的
**软件栈验证阶段**：先把 FileX + LevelX 和"采集 → 落盘 → 回读校验"这条链路
跑通，再谈真实介质。当前链路：TIM3 触发 ADC1 → **GPDMA 循环缓冲或 EOC 中断**
→ 环形缓冲 → 窗口统计 → CSV 追加 → 2 KB 轮转 → `acq check` 逐行回读校验。
默认 10 kHz，DMA 模式实测 50 kHz 零丢弃（`acq start dma 50000`）；再往上瓶颈是写盘而不是采样。

## 为什么要模拟盘

板载没有 SD 卡座，也没有 SPI NOR。等有了外部介质再写文件系统，问题和介质问题会
混在一起。模拟盘把两件事拆开：

- **现在**：在 RAM 里模拟 NOR 的物理约束（只能整扇区擦、擦前必须 `0xFF`、
  写入只能 1→0），完整跑 FileX + LevelX 的 FAT、扇区映射、磨损均衡和块回收。
- **以后**：接真实 SPI NOR / SD 时，只替换 `app_fxdisk.c` 里的底层驱动，
  FileX 与 LevelX 之上的应用代码一行不改。

模拟盘是**易失**的：断电或复位后内容丢失，每次启动会重新格式化。这是刻意设计，
避免与"掉电持久化"混淆（后者见
[Control-Node 的片内 Flash 参数页](../NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md)）。

## 已实现

| 层 | 实现 | 说明 |
| --- | --- | --- |
| 应用层 | `fs` 命令族 | `info/ls/format/mount/umount/write/append/read/rm/test/fill/stress` |
| 文件系统 | FileX（Cube 包 `common/src` 全量） | FAT 卷 `SIMDISK`，120 个 512 B 扇区，64 个根目录项 |
| 磨损均衡 | LevelX NOR | 逻辑扇区→物理扇区映射、作废标记、块回收 |
| 物理介质 | `app_fxdisk.c` 中的 RAM 模拟 NOR | 8 块 × 16 扇区 × 512 B = 64 KiB；每块 1 扇区做元数据，可用 120 逻辑扇区 |
| 观测 | `FSD` 状态行 | 驱动读/写次数、LevelX 块分配/作废/空闲、每块擦除计数、**1→0 违规计数** |
| 采集 | TIM3 10 kHz 触发 ADC1（PC0/A1），EOC 中断写入 1024 点环形缓冲 | 采样线程按 50 ms 窗口统计 min/max/avg，追加到 CSV |
| 落盘 | `LOGnnnn.CSV`（序号,min,max,avg,count），2 KB 轮转，保留 4 个文件 | 复用上面的 FileX/LevelX 盘；文件句柄常开，关闭时 `fx_media_flush` |
| 回读校验 | `acq check` 逐行解析最新日志 | 校验字段数、`min ≤ avg ≤ max`、最大值 ≤ 4095、每行 500 样本 |

`violations=0` 是关键指标：它统计"尝试把 0 写成 1"的次数。恒为 0 说明 LevelX
每次都先擦除再写，没有依赖"RAM 可以任意覆盖"的假象。

## 板上已验证（实测）

| 项目 | 结果 |
| --- | --- |
| 构建 | `text 218,492 / data 2,380 / bss 109,384`，`.bin` 220,876 B（模拟 NOR 占 64 KiB bss） |
| 启动 | `FSD mount=formatted sectors=120 reason=1` → `READY tx-filex-simdisk 1.0.0` |
| 挂载信息 | `FSD mounted=1 media=SIMDISK sectors=120 sector_bytes=512 free_bytes=58368` |
| 写读闭环 | `fs write DEMO.TXT hello-simdisk` → 回读 `text=hello-simdisk`；`append` 后 `hello-simdisk+more` |
| 目录操作 | `fs ls` → `FS f=DEMO.TXT size=13`；`fs rm` 后不再列出 |
| 大文件 | `fs fill 32`（32 KiB）写入并逐字节校验 PASS；`free_bytes 58368 → 25600` |
| 磨损均衡 | `fs fill 32` 后 `erases=5`，擦除计数分散在 e1/e2/e3/e4/e5 |
| 重复改写 | `fs stress 60`：127 次扇区写、123 次块分配、93 个作废扇区、2 次块擦除，`violations=0`，校验全通过 |
| 卸载/重挂 | `fs umount` → `fs mount` 后数据仍在（`FS f=FILL.BIN`） |
| 重新格式化 | `fs format` → `FS format=ok rc=0`，`fs count=0` |
| 采集（中断） | `acq start irq 10000` 9.2 s：`windows=184 samples=92000 drop=0` |
| 采集（DMA） | `acq start dma` 3.2 s：`windows=64 samples=32000 drop=0 dma_ovr=0` |
| 高采样率 | `acq start dma 50000` 4.2 s：`windows=422 samples=211000 drop=0 dma_ovr=0`，5 倍速率零丢弃 |
| 文件轮转 | 50 kHz 采集 4.2 s 写满 `LOG0000..LOG0003.CSV`（各 2 KB），`files=4` |
| 写盘瓶颈 | `acq start dma 100000` 3.2 s：只落下 `samples=162500`、`drop=160049`，采样线程自己写盘已跟不上——下一步要解耦采集与落盘 |
| 回读校验 | `acq check` → `ACQ CHECK PASS lines=81 samples=40500 min=299 max=528` |
| GPDMA 诊断 | `dma test` 10 个用例：只有「安全通道 + 特权 + 安全源/目拍」落地，`DMA MEMTEST PASS` |
| 线程栈 | `sampler=1400/2048`、`console=1364/4096`、`monitor=1008/2048` |
| 一键自检 | `./scripts/selfcheck.sh all`：`basic` 10、`fs` 19、`acq` 27、`dma` 19、`watchdog` 4、`fault` 5 全部 PASS |

## 串口命令

USART3（ST-LINK 虚拟串口，PD8/PD9，115200 8N1）。可靠性层命令
（`status/stack/fault/wdg/log/stall/crash/reboot`）见
[Reliability-Monitor README](../NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)。
新增：

| 命令 | 作用 |
| --- | --- |
| `fs` / `fs info` | 容量、剩余空间、驱动读写次数、LevelX 与擦除统计 |
| `fs ls` | 列目录，末行 `FS count=N` |
| `fs write <name> <text>` | 创建/覆盖文件并写入文本 |
| `fs append <name> <text>` | 追加，用于验证"同扇区改写"路径 |
| `fs read <name>` | 回读并打印 `FS DATA name=... text=...` |
| `fs rm <name>` | 删除文件 |
| `fs test` | 写→读→比对→删→确认读失败，输出 `FS SELFTEST PASS/FAIL` |
| `fs fill <1..48>` | 写入 N KiB 伪随机图案并逐字节校验，触发多块分配与擦除 |
| `fs stress <1..500>` | 重复"覆盖写 + 校验"N 次，驱动 LevelX 块回收与磨损均衡 |
| `fs format` / `fs mount` / `fs umount` | 重新格式化 / 挂载 / 卸载（采集中拒绝执行） |
| `acq start [dma\|irq] [hz]` | 开始采集：`irq`=EOC 中断（默认），`dma`=GPDMA 循环；`hz` 1000..100000 |
| `acq stop` | 停止采集并关闭日志文件 |
| `acq info` | 运行状态、模式、采样率、窗口数、样本数、文件数、min/max、`drop`、`dma_ovr`，DMA 模式下附通道 1 寄存器 |
| `acq check` | 回读最新日志并逐行校验，输出 `ACQ CHECK PASS/FAIL` |
| `acq selftest` | 采集 1.5 s → 停止 → 校验，一条命令验证整条链路 |
| `dma env` | 打印 SAU/GTZC MPCBB 的安全与特权归属、GPDMA1 通道属性 |
| `dma test` | 10 个 mem2mem 用例矩阵（属性/端口/存储体/别名），输出 `DMA MEMTEST PASS/FAIL` |

## 构建与烧写

```sh
make clean
make -j4
make flash
```

需要 `CUBE_FW`（默认 `/mnt/study/mcu_fw/stm32CubeH5/STM32Cube_FW_H5_V1.7.0`）、
ARM GNU 工具链和 OpenOCD。Makefile 引入：

- `Middlewares/ST/filex/common/src/*.c`（全量）与 `filex/ports/generic/inc`
  —— FileX 没有 cortex_m33 专用 port，用 generic 可移植 port；
- `Middlewares/ST/levelx/common/src/lx_nor_flash_*.c`，**排除**
  `lx_nor_flash_simulator.c`（本工程自带模拟 NOR 后端，尺寸和语义可控）。

## 一键自检

```sh
./scripts/selfcheck.sh basic     # 构建 + 符号 + 烧写 + 状态/栈/故障/日志
./scripts/selfcheck.sh fs        # 文件系统 19 项断言
./scripts/selfcheck.sh acq       # 中断 + DMA 两种采集→落盘→回读 27 项断言
./scripts/selfcheck.sh dma       # GPDMA 属性矩阵 + 测试后采集中断不被破坏 19 项断言
./scripts/selfcheck.sh watchdog  # 线程卡死 → IWDG 复位
./scripts/selfcheck.sh fault     # BusFault 记录与重启报告
./scripts/selfcheck.sh all       # 六组顺序执行
```

可用 `PORT=/dev/ttyACM0` 覆盖串口。

## 目录结构

```text
Core/Inc/     app_fxdisk.h（盘接口）+ P1 继承的头文件
Core/Src/     app_fxdisk.c（FileX 驱动 + LevelX 配置 + RAM 模拟 NOR）
              main.c, app_threadx.c, app_log.c, app_console.c,
              app_fault.c, app_watchdog.c, app_stacks.c
openocd/      st_nucleo_h5.cfg (dapdirect) + stm32h5x.cfg (官方 target)
scripts/      selfcheck.sh, serial_check.py
```

## 设计说明

- **几何**：8 块 × 16 物理扇区，每块第 0 扇区被 LevelX 用作块元数据
  （擦除计数、空闲位图、扇区映射），因此可用 120 个逻辑扇区 = 60 KiB。
- **FileX 驱动**：实现 `FX_DRIVER_READ/WRITE/RELEASE_SECTORS/FLUSH/ABORT/INIT/
  UNINIT/BOOT_READ/BOOT_WRITE`，读写映射到
  `lx_nor_flash_sector_read/write/release`；`INIT` 打开 LevelX，`UNINIT` 关闭。
- **`free_sector_update = FX_TRUE`**：FileX 在扇区不再使用时通知驱动，
  LevelX 才能把旧逻辑扇区标记作废并回收。
- **BOOT_READ 校验**：读 0 号扇区检查 `EB 34 90` 跳转指令，无效返回
  `FX_MEDIA_INVALID`，上层据此决定是否格式化。
- **写入语义**：模拟 NOR 写时执行 `value &= current` 并统计违规次数。
  LevelX 正确工作时违规恒为 0。
- **缓存陷阱（实测踩到）**：FileX 会把同一扇区长期留在 `media_memory` 缓存，
  只做 `truncate + write + close` 时 20 次改写可能只产生 1 次真实扇区写。
  因此写入函数在关闭文件后调用 `fx_media_flush`，压力测试才真的落到"闪存"。
- **初始化时机**：`fx_system_initialize()` 会创建 FileX 系统定时器，必须在
  `tx_application_define()` 里调用；格式化/挂载放在 console 线程入口，
  避免在内核对象创建阶段做 I/O。
- **采集不用 DMA**：TIM3 10 kHz 产生 TRGO 触发 ADC1 常规组，EOC 中断只做
  "读 DR + 入环形缓冲"（1024 点）。采样线程每 50 ms 取 500 点统计后落盘。
  这样控制环/日志线程不受影响，也避开了已验证失败但根因未定的 GPDMA 路径
  （详见 [report.md](./report.md)）。

## 已知限制

- 模拟盘**掉电即失**，每次启动重新格式化，不能用来验证掉电一致性。
- 单扇区擦除在 RAM 里是"瞬间"的，测不出真实 NOR 的擦除延时和擦除期间的取指停顿。
- 64 KiB 容量只为验证栈，不代表产品容量规划；真实介质要重算簇大小与 FAT 类型。
- 没接 RTC，文件时间戳是 FileX 的固定初始值。
- `fs fill` / `fs stress` 是调试命令，量产固件应通过编译开关裁掉（尚未实现）。

## 文档

- [Application Note](../Application_note.md)：方向、路线与验收标准
- [report.md](./report.md)：实现细节与踩坑复盘
- [Reliability-Monitor README](../NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)：继承的可靠性层
