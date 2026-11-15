# NUCLEO-H563ZI ThreadX Tx-FileX-SimDisk 复盘

## 目标

板载没有 SD 卡座和外部 Flash，但 [Application Note](../Application_note.md) 路线 2
（数据记录仪）需要 FileX + LevelX。做法是在片内 RAM 里模拟一块 NOR 闪存，
让完整的文件系统栈先跑起来：

- 不依赖任何外部介质即可验证 FileX API、FAT 布局、目录操作；
- 模拟真实 NOR 的物理约束，验证 LevelX 的扇区映射与磨损均衡确实在工作；
- 将来接真实 SPI NOR / SD 时只替换底层驱动。

底座从 `Tx-Reliability-Monitor` 复制，保留日志、看门狗、故障记录、状态台和自检。

## 结果摘要

- 构建：`text 146,844 / data 2,364 / bss 99,408`，`.bin` 149,216 B；
  其中 `app_fxdisk.o` 的 bss 为 77,528 B（模拟 NOR 64 KiB + 缓冲）。
- 启动：`FSD mount=formatted sectors=120 reason=1`（`reason=1` 即
  `FX_BOOT_ERROR`：空白 RAM 盘首次挂载失败后自动格式化）。
- 卷：`media=SIMDISK sectors=120 sector_bytes=512 free_bytes=58368`。
- `fs test`：写→读→比对→删除→确认读失败，全部 PASS。
- `fs fill 32`：32 KiB 写入 + 逐字节校验 PASS；`free_bytes` 58368 → 25600；
  `erases=5`，擦除计数分散到 e1/e2/e3/e4/e5。
- `fs stress 60`：127 次真实扇区写、123 次块分配、93 个作废扇区、2 次块擦除，
  每一步都读回校验，`violations=0`。
- `fs umount` → `fs mount` 后数据仍在；`fs format` 后目录清空。
- 采集 → 落盘：`acq start` 3 s → `windows=61 samples=30500 drop=0`（≈10 kHz）；
  日志行 `0,467,810,519,500`（序号, min, max, avg, count）。
- 文件轮转：9 s 采集写出 `LOG0000.CSV`(2053 B) + `LOG0001.CSV`(1617 B)。
- 回读校验：`acq check` → `ACQ CHECK PASS lines=77 samples=38500 min=325 max=545`。
- 线程栈：`sampler=1376/2048`、`console=1480/4096`、`monitor=1008/2048`。
- `./scripts/selfcheck.sh all`：`basic` 9、`fs` 19、`acq` 14、`watchdog` 4、
  `fault` 5 全部 PASS。

## 实现

### 组件与几何

- FileX：`Middlewares/ST/filex/common/src/*.c` 全量编译，port 用
  `ports/generic/inc`（该包没有 cortex_m33 port）。
- LevelX：只编译 `lx_nor_flash_*.c`，并排除 `lx_nor_flash_simulator.c`，
  改用本工程自己的模拟后端，便于控制容量和统计。
- 模拟 NOR：8 块 × 16 物理扇区 × 512 B。LevelX 每块占 1 个物理扇区做元数据
  （擦除计数、空闲位图、扇区映射表），因此可用 120 个逻辑扇区。
- 卷参数：1 个 FAT、64 个根目录项、1 扇区/簇、扇区 512 B。

### FileX 驱动映射

| FileX 请求 | 实现 |
| --- | --- |
| `FX_DRIVER_INIT` | `lx_nor_flash_open()`，并置 `free_sector_update = FX_TRUE` |
| `FX_DRIVER_UNINIT` | `lx_nor_flash_close()` |
| `FX_DRIVER_READ` / `WRITE` | 逐扇区 `lx_nor_flash_sector_read/write` |
| `FX_DRIVER_RELEASE_SECTORS` | `lx_nor_flash_sector_release`，让 LevelX 标记作废 |
| `FX_DRIVER_BOOT_READ` | 读 0 号扇区并校验 `EB 34 90` |
| `FX_DRIVER_FLUSH` / `ABORT` / `BOOT_WRITE` | 直接成功 / 写 0 号扇区 |

### 模拟 NOR 的写入语义

真实 NOR 只能把 1 写成 0，必须整块擦除才能恢复 1。模拟后端写入时执行：

```c
if ((value & ~current) != 0U) { sim_bit_violations++; value &= current; }
```

`violations` 不作为错误返回，只做观测——如果 LevelX 逻辑正常，它应该恒为 0。
实测 `fs fill 32` 与 `fs stress 60` 之后都是 0，说明确实每次先擦后写。

## 采集链路

### GPDMA 不落内存的根因（已定位）

原计划 TIM3 触发 ADC + GPDMA 循环搬运。第一阶段能启动、能计数，但缓冲区内容不变，
当时先退回 EOC 中断保证功能可用。随后用 `dma test`（纯 mem2mem，不经过 ADC/FileX）
把问题单独复现并定位。

**根因：GPDMA 的安全属性有两层，之前只设了第一层。**

| 层级 | 寄存器 | HAL 参数 | 作用 |
| --- | --- | --- | --- |
| 通道级 | `GPDMA_SECCFGR` / `PRIVCFGR` | `DMA_CHANNEL_SEC` / `DMA_CHANNEL_PRIV` | 通道本身以什么安全级/特权级发起访问 |
| 传输级（每个 beat） | `CCR`… 实为通道 `CTR1.SSEC` / `CTR1.DSEC`；链表模式下是节点里的 `SrcSecure` / `DestSecure` | `DMA_CHANNEL_SRC_SEC` / `DMA_CHANNEL_DEST_SEC` | 源/目的**数据拍**是安全访问还是非安全访问 |

`dma env` 在本板实测：

```
DMA ENV sau_ctrl=00000000 sau_type=00000008 allns=0   # SAU 未启用 => 全部安全
DMA ENV gpdma1 seccfgr=00000000 privcfgr=00000000     # 通道默认非安全 + 非特权
DMA ENV src=20019200 sram1 sec=1 priv=1               # SRAM 块：安全 + 仅特权可访问
```

也就是说 SRAM 在 GTZC MPCBB 里是「安全 + 特权」的，而 GPDMA 通道复位后是「非安全 +
非特权」，且**即使把通道改安全，数据拍仍然是非安全的**。TZSC 直接把这种访问过滤掉：
通道照常计数、照常产生 HT/TC 中断、**任何错误标志都不置位**，只有数据没了。这与最初
"计数但不落内存"的现象完全一致；ADC 侧日志出现全 0 样本同理（读安全别名
`0x5202_8040` 的非安全访问被过滤，读回 0）。

`dma test` 的判定矩阵（32 word mem2mem，`attr` 缩写：P=特权通道 S=安全通道
SS=安全源 DS=安全目的）：

| # | attr | 缓冲区 | 结果 | 说明 |
| --- | --- | --- | --- | --- |
| 00 | `-`（复位默认） | SRAM1 | FAIL mism=32 | 非安全+非特权通道，全部被过滤 |
| 01 | P | SRAM1 | FAIL | 只加特权不够 |
| 02 | PS | SRAM1 | FAIL | **通道安全也不够**（数据拍仍非安全）|
| 03 | PSSD | SRAM1 | **PASS** | 通道安全+特权，源/目拍安全 |
| 04 | SSD | SRAM1 | FAIL | 缺特权（MPCBB 要求 priv=1）|
| 05 | PSD | SRAM1 | FAIL | 缺通道级安全 |
| 06 | PSSD | SRAM1（PORT0/PORT0）| PASS | 端口分配不影响结论 |
| 07 | PSSD | SRAM2 | PASS | |
| 08 | PSSD | SRAM3 | PASS | |
| 09 | PSSD | 安全别名 `0x3000_xxxx` | PASS | 两种别名等价 |

所有失败用例的 `csr=0001`、`err=0000`、`cbr1=0000`：**传输"成功"完成，只是数据被丢弃**。
这就是为什么当初只看错误码查不出问题。

对应的正确写法（本工程采用，链表模式）：

```c
/* 通道级 */
HAL_DMA_ConfigChannelAttributes(&hdma, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC);
/* 节点级（H5 HAL 用节点字段表达 SSEC/DSEC） */
node.SrcSecure  = DMA_CHANNEL_SRC_SEC;
node.DestSecure = DMA_CHANNEL_DEST_SEC;
```

ADC DR 走安全别名（`ADC1_S` → `0x5202_8040`），所以源侧必须是安全拍。

### 现在的采集实现

两条路径可在运行时选择，共用同一个 1024 点环形缓冲和窗口逻辑：

- **IRQ 路径**（`acq start irq`）：TIM3 TRGO → ADC1 → EOC 中断里读 DR 入环。
  简单、CPU 占用随采样率线性上升。
- **GPDMA 路径**（`acq start dma [hz]`）：单节点链表 + `DMA_LINKEDLIST_CIRCULAR`，
  ADC 请求 → GPDMA 循环写入 `2 × 512` 半字缓冲。HT/TC 中断只置"半满就绪"位，
  采样线程把就绪的一半搬进环形缓冲。GPDMA 没有独立循环位，循环靠"尾节点指回首节点"。

`acq info` 会额外打印通道 1 的原始寄存器，便于现场判断：

```
ACQ running=1 mode=dma rate=50000 file=LOG0001.CSV windows=173 samples=86500     files=2 min=500 max=825 drop=469 dma_ovr=0
ACQ dma ch1 ccr=00805f01 csr=00000000 cb1=00000434 sar=52028040 dar=20018e0c
```

`dma_ovr` 是"DMA 半满时上一半还没被取走"的次数；`drop` 是环形缓冲溢出。
环形缓冲最初 1024 点（一次 feed 最多推进 2 × 512 = 1024 点，必然溢出），
改成 2048 点后：

| 配置 | 时长 | 结果 |
| --- | --- | --- |
| `acq start irq 10000` | 9.2 s | `samples=92000 drop=0` |
| `acq start dma 50000` | 4.2 s | `samples=211000 drop=0 dma_ovr=0`，写满 4 个日志文件 |
| `acq start dma 100000` | 3.2 s | `samples=162500 drop=160049 dma_ovr=1`——掉一半 |

100 kHz 时 `dma_ovr` 只多了 1 次，说明 DMA 侧几乎没被饿死，丢失全部来自环形缓冲：
采样线程既搬数据又同步写文件，单窗口 `fx_media_flush()` 的耗时已经把吞吐压在
≈50 k 样本/秒。**结论：采样不再是瓶颈，写盘是**；要更高采样率必须先解耦
"采集线程只入队 / writer 线程落盘"。

`dma test` 会重置 **GPDMA1 通道 0**（只有 mem2mem 用），因此 `acq start dma` 每次启动
都会重新建立通道 1 的链表与属性——即使中途被 `dma test` 之类操作打断也能自愈。

### 记录格式与轮转

- 每 50 ms 一行：`序号,min,max,avg,count`，例如 `0,467,810,519,500`。
- 单文件 2 KB 轮转，文件名 `LOG0000.CSV`…`LOG0003.CSV` 循环覆盖，最多 4 个文件。
- 文件句柄常开（不每次 open/close），写入后靠 `fx_media_flush` 落盘；
  采集中禁止 `fs format` / `fs umount`，避免把正在写的卷拆掉。

### 回读校验

`acq check` 用 `app_fxdisk_read_stream` 流式读取最新日志，逐行校验：
字段数必须为 5、`min ≤ avg ≤ max`、`max ≤ 4095`、`count` 必须等于 500。
任何一行不满足即输出 `ACQ CHECK FAIL ... line=N`，便于定位是第几行坏了。

## 踩到的问题

### 1. FileX 没有 cortex_m33 port

`fx_api.h` 需要 `fx_port.h`，包内只有 cortex_m0/m3/m4/m7、generic、linux、win32。
H5 是 Cortex-M33，直接用 `ports/generic/inc` 即可（FileX 的可移植 port 不依赖
具体内核指令）。

### 2. 空白 RAM 盘首次挂载返回 `FX_BOOT_ERROR`

RAM 上电是 0x00 而不是擦除态 0xFF。模拟后端在第一次初始化时把整片填成
`0xFFFFFFFF`（模拟出厂擦除态），随后 `BOOT_READ` 读到的仍不是 FAT 引导记录，
于是 `fx_media_open` 失败，返回 `reason=1`。上层捕获后调用 `fx_media_format`
再挂载——这就是真实产品里"新盘需要格式化"的同一条路径。

### 3. FileX 扇区缓存让压力测试失真（最重要的坑）

最初 `fs stress` 的语义是"truncate → write 512 B → close"重复 N 次。
实测 60 次改写只产生了 **1 次**真实扇区写，`erases` 一直是 0，
看起来"磨损均衡没工作"。

原因是 FileX 把该文件所在的逻辑扇区一直留在 `media_memory` 缓存里：
改写只改了缓存内容，`fx_file_close()` 也不强制落盘，缓存不淘汰就永远不写介质。

修复：写入路径在 `fx_file_close()` 后显式 `fx_media_flush()`。此后：

| 指标 | 加 flush 前 | 加 flush 后 |
| --- | --- | --- |
| 60 次改写的驱动扇区写 | 1 | 127 |
| LevelX 块分配 | +1 | 123 |
| 作废扇区 | 1 | 93 |
| 块擦除 | 0 | 2 |

**教训**：验证磨损均衡/擦除行为时，必须确认写请求真的到达驱动层，
否则测的是缓存而不是闪存。为此在驱动里加了 `reads/writes/sectors` 计数，
`fs info` 直接可见。

### 4. `fx_system_initialize()` 必须在核内上下文调用

它内部会 `tx_timer_create` 创建 FileX 系统定时器，只能在
`tx_application_define()` 这类内核上下文里做。格式化/挂载则放到 console 线程
入口，避免在"还没调度"的阶段做 I/O。

### 5. `fs` 命令解析与栈深度

`fs write <name> <text>` 需要切分参数，用了 64 B 名 + 512 B 数据的静态缓冲，
避免占用 console 线程栈；同时把 console 栈从 3072 B 提到 4096 B，
给 FileX → LevelX 的调用链留余量。实测 `STACK console=` 仍有余量。

### 6. 厂商源码的一条 warning

编译 `fx_media_format.c` 会报 `-Wunterminated-string-initialization`（卷标初值
9 字节入 8 字节数组）。这是上游 6.x 源码自带的写法，本工程没有改厂商代码，
也不影响功能；如果要求零 warning，可对 FileX 源单独加
`-Wno-unterminated-string-initialization`。

## 尚未覆盖

- **高采样率下的写盘瓶颈**：50 kHz 时采样已经不吃 CPU，但每 500 样本仍要写一行、
  每 2 KB 轮转一次，落盘抖动会造成约 1% 的环形缓冲丢弃。要真跑更高采样率必须先改
  存储策略（批量写 / 更大的窗口 / 二进制记录）。
- **采集与日志解耦**：当前采样线程自己写文件，写 Flash 期间该线程被占用；
  真实产品应改成"采样线程只入队，独立 writer 线程落盘"。
- **时间戳**：日志只有窗口序号，没有绝对时间；接 RTC 后应记录起始时间。
- **掉电一致性**：RAM 盘复位即失，这一层无法验证。真实介质阶段需要单独设计
  （日志式写入 / 双索引 / 允许丢尾部 N 秒）并做断电注入。
- **擦除延时与取指停顿**：RAM 擦除是瞬时完成的，测不出真实 NOR 的擦除时间，
  也测不出擦除期间同 bank 取指的停顿。
- **容量与性能**：60 KiB 只能验证功能；真实介质要按容量重算簇大小、FAT 类型，
  并测写入吞吐。
- **并发**：采样线程写日志、console 线程做 `fs` 操作，两者共用 FileX 媒体互斥；
  已验证不会互相破坏，但没有做高并发压力测试。

## 下一步

1. 有 SPI NOR / SD 卡后，替换 `app_fxdisk.c` 的底层读写为真实驱动，
   保持 FileX/LevelX 层不变，先跑同一套 `fs` + `acq` + `dma` 自检。
2. 采集与落盘解耦（采样线程只入队、独立 writer 线程落盘），把 50 kHz 以上时
   约 1% 的丢弃消掉。
3. 加 RTC 时间戳与文件头（起始时间、采样率、通道），让日志能脱离固件解读。
4. 真实介质上做掉电注入与介质寿命评估。
