# NUCLEO-H563ZI_THREADX_Tx-Usb-Data-Logger

**USB 仪器化数据记录仪：ADC 多通道采集 → CSV 落盘 → USB-C 导出（MSC）+ 命令通道（CDC）。**

从 [`Tx-Data-Logger`](../NUCLEO-H563ZI_THREADX_Tx-Data-Logger/README.md)（提交
`c73626d`）派生，原样保留它已验证的 ADC 序列扫描、GPDMA 交织、FileX+LevelX、
SD（SPI 模式）、RTC 时间戳与八组自检，在此基础上加 **USBX CDC 命令通道**
（MSC 数据盘是下一步）。

对应 [Application Note](../Application_note.md) 路线 2 的 P3：把"拔卡取数 +
UART 命令"升级为"一根 USB-C 同时提供 MSC 数据盘与 CDC 命令通道"。

> **当前状态：P3-1（CDC 命令通道）代码完成。** USBX 设备栈、CDC-ACM 类、
> `usb`/`dump` 命令与 `usb` 自检组已就位；其余八组自检在链接了 USBX 后仍全
> PASS。等待插线验证枚举与端到端导出。

## 与上游 Tx-Data-Logger 的关系

| 方面 | Tx-Data-Logger（冻结基线） | 本工程 |
| --- | --- | --- |
| 取数方式 | 拔 SD 卡 + 读卡器 | USB-C MSC 数据盘，直接拷（P3-2） |
| 命令通道 | USART3（ST-LINK VCP，115200） | 并行：USART3 + CDC-ACM 虚拟串口（P3-1） |
| 文件导出 | 读卡器 | MSC 只读挂载，或 CDC `dump` 流式导出 |
| 结构 | — | 加 USBX core + DCD + CDC/MSC 类，PA11/PA12，HSI48 |

上游工程保持不动，作为"SD + RTC 全部验证通过"的参考实现；两边共享同一套
采集、存储与自检代码，改动需有意同步（见文末"与上游同步"）。

## 与单通道基线的关系

| 方面 | SimDisk（单通道，已归档） | 本工程 |
| --- | --- | --- |
| 通道 | ADC1_INP10 / PC0 | ADC1_INP10..13 / PC0..PC3（1–4 路可选） |
| 转换方式 | 每次触发 1 次转换 | 每次触发转换整个 rank 序列 |
| 中断路径 | 可用 | **仅单通道**（多通道必须用 DMA，原因见下） |
| 环形缓冲 | 2048 样本 | 8192 样本（交织存放） |
| CSV | `idx,min,max,avg,count` | `idx,count,nch,min0,max0,avg0[,min1,...]` |
| 文件大小 | 2 KB | 4 KB（多通道行长约为 4 倍） |
| 自检 | 七组 | 八组（新增 `rtc`） |

## 通道表

| 序号 | ADC 通道 | 引脚 | 备注 |
| --- | --- | --- | --- |
| 1 | `ADC_CHANNEL_10` | PC0 | Arduino A1 / CN9，**唯一接过信号验证的口** |
| 2 | `ADC_CHANNEL_11` | PC1 | 与 PC0 相邻的排针脚位 |
| 3 | `ADC_CHANNEL_12` | PC2 | |
| 4 | `ADC_CHANNEL_13` | PC3 | |

四路都在 GPIOC 上，`HAL_ADC_MspInit()` 一次性把 `PIN_0..PIN_3` 配成模拟输入。
用 `acq ch <1..4>` 选择通道数，选中的是**表里的前 N 个**，不是任意组合。

注意排针位置：这块板 Arduino 模拟排针是 **A1=PC0、A2=PC3、A4=PC2、A5=PF11**，
**PC1 不在 Arduino 排针上**（只在 Morpho/Zio 上）。按 A4/A5 接线会落到 PC2/PF11。

### 外部模拟通道当前读不到电压（未解决）

2026-11-15 实测：把 PC1 接 GND、PC2 接 3V3 后，`acq` 的四路读数仍是同一水平的
约 1.2 V（12 bit 约 1470–1505），与引脚电压无关。逐项排查结论：

- 固件配置正确：`SQR1` 解码为 rank1–4 = 通道 10/11/12/13，
  `CFGR` 外部触发 = TIM3_TRGO 上升沿、12 bit；GPDMA 链表节点源地址 =
  `ADC1->DR`（`0x52028040`）、目的 = 环形缓冲、长度 8000 字节、自环。
- 引脚确实被外部拉住了：用调试器把引脚切成输入 + 内部上拉/下拉并回读 `IDR`，
  PC1 读 0/0（被拉低）、PC2 读 1/1（被拉高），换线后随之变化。
- ADC 内核与基准正常：`acq level` 读到 VREFINT ≈ 1505（反推 VDD = 3.29 V）、
  内部温度传感器 ≈ 752（604 mV，室温合理）。
- 只有**外部通道**不行：PC0/PA6（接 GND）、PB1（接 3V3）、PC3 全部读 ~1.2 V；
  连芯片内部 DAC 驱动 PA4（`DAC1_OUT1` = `ADC1_INP18`）也读不出 0/2048/4095。

也就是说：内部通道能读到真实电压，外部通道一律读同一个约 1.2 V 的悬空电平。
线接在哪、通道号怎么配都不影响这个结果，说明问题在“焊盘 → ADC 输入”这一段
（硬件或板级），不是本工程的软件配置。`acq level` / `acq dac` 就是为此加入的
诊断命令，可随时复测。

#### 2026-11-16 交叉验证（结论：芯片外部模拟通路不工作）

三条互相独立的证据链：

1. **官方例程对照**。用 ARM GCC 按源码清单构建 ST 的
   `Projects/NUCLEO-H563ZI/Examples/ADC/ADC_SingleConversion_TriggerSW_DMA`
   并烧写：程序正常运行（LED 闪烁），但缓冲区一直是初值 4096、
   `ubDmaTransferStatus=2` —— GPDMA 一个 beat 都没落地。原因与 `b583395`
   同一类：TrustZone 下 SRAM 标为安全，而官方例程没有设置**数据拍级**安全属性
   （`SrcSecure`/`DestSecure`），传输被 GTZC 静默过滤。因此官方例程不能直接
   用来判定引脚，必须换成不经过 DMA 的轮询方式。
2. **DAC → 同一焊盘回环**。自包含轮询程序（沿用官方例程的时钟/ADC 配置）：
   `DAC1_OUT1` 驱动 PA4，`ADC1_INP18` 读回同一焊盘。
   - 单次转换看似单调（DAC 0/1024/2048/3072/4095 → 1031/1212/1264/1279/1286）
   - 但**连续 64 次转换后，DAC=0 收敛到 1603、DAC=4095 收敛到 1601** —— 完全相同，
     说明“单调”只是采样电容残留上一次电平的瞬态
   - 同一时刻用数字输入回读 PA4 焊盘：DAC=0 → `IDR` 位为 **0**，
     DAC=4095 → **1**，证明**焊盘电平本身是对的**，是 ADC 看不到它
3. **20 个通道全扫**（PC0 接 3V3、PC3 接 GND，先用上拉/下拉回读确认引脚确实被外部驱动）：

   | 通道 | 引脚 | 实际电压 | 读数 | 期望 |
   | --- | --- | --- | --- | --- |
   | 10 | PC0 (A1) | 3.3 V | 1494 | ~4095 |
   | 13 | PC3 (A2) | 0 V | 1498 | ~0 |
   | 16 | 温度（内部） | — | 748 | 合理 |
   | 17 | VREFINT（内部） | — | 1506 | 正确 |
   | 其余 16 个外部通道 | PA/PB/PC | 未接 | 1473–1533 | — |

结论：**内部通道正常、外部通道全部锁在约 1.2–1.3 V，且横跨 PA/PB/PC 三个端口
共 7 个引脚**。这不是单脚损伤，而是该芯片的外部模拟输入通路（焊盘 → ADC
多路开关/采样网络）整体不工作。下一步待确认 VDDA/VREF+ 实测电压，或换一块
NUCLEO-H563ZI 复测。

> 附带修正：`Nx-Telemetry-Node` 把 `ADC_CHANNEL_3` 注释成 PA3，但
> **PA3 = ADC1_INP15，INP3 是 PA6**，它实际采的是 PA6。

#### 2026-11-17 补充排查（软件侧已排除干净）

为了排除"只有 ADC1 或只有规则组坏了"，又做了三组独立实验，全部无效：

| 实验 | 方法 | 结果 |
| --- | --- | --- |
| 换 ADC2 | `DAC1_OUT1` 驱动 PA4，`ADC2_INP18` 读同一焊盘 | DAC 0→4095 时读数 986→770，**反向且不跟随** |
| 换注入组 | ADC1 注入组读 PA4（Control-Node 用的就是注入组） | `DAC=4095` 与 `DAC=0` 都是 **0x4fb/0x4fc**，完全相同 |
| 模拟开关 booster | 固件里置 `SBS_PMCR.BOOSTEN` | 该位读回仍为 0，读数无任何变化 |

同时确认 `SBS_CCCSR`（IO 补偿单元）也是 0（未使能），置位无效。

三组实验的共同结论：**规则组/注入组、ADC1/ADC2 全部读不到外部电压**，
而两个 ADC 各自的内部通道都正常（ADC1 的 VREFINT=1507、温度=752；
ADC2 的 VDDCORE≈1692）。读数会"缓慢收敛"到某个固定值，是采样电容
没有被输入驱动的典型表现。

#### 2026-11-17 复验（带真实外部电压）

之后的讨论指出了一个方法问题：**浮空引脚读到 ~1.2 V 本身是正常的**，不能作为
证据；而 DAC 回环里 PA4 同时是 `DAC1_OUT1` 和 `ADC1_INP18`，DAC 使能后该引脚的
模拟开关被占用，那组数据也不够硬。所以用外部电压重做了一次干净验证：

1. 接线：3V3 → A1(PC0)，GND → A2(PC3)
2. 调试器上拉/下拉回读确认引脚电压正确：
   `PC0 driven HIGH (VDD)`、`PC3 driven LOW (GND)`、PC1/PC2 浮空
3. 立刻用 `acq level` 读：

   | 通道 | 引脚 | 外部电压 | 读数 | 应该是 |
   | --- | --- | --- | --- | --- |
   | 10 | PC0 | 3.3 V | **1493** | 4095 |
   | 13 | PC3 | 0 V | **1497** | 0 |
   | 11 | PC1 | 浮空 | 1481 | — |

   接 3.3 V、接 0 V、浮空，三者读数没有区别。

4. 再扫采样窗口（怀疑采样电容充不满）：把 ADC 内核时钟换成 HSI 异步并分频，
   640.5 周期从 10 µs 拉长到 **2.56 ms**（长 250 倍）：

   | ADC 时钟 | 连续 4 次读数 |
   | --- | --- |
   | HSI/1 | 1106 / 1400 / 1523 / 1570 |
   | HSI/16 | 1522 / 1537 / 1545 / 1550 |
   | HSI/64 | 1416 / 1410 / 1396 / 1399 |
   | HSI/256 | 1037 / 929 / 900 / 887 |

   读数随窗口变化但**从不接近 4095**，且方向不一致（慢时钟反而更低），
   说明采样节点被内部的泄漏/偏置主导，而不是被输入驱动。

**判定：这块芯片的 IO 模拟输入通路（焊盘 → ADC 采样网络）损坏。**
换引脚、换 ADC（ADC1/ADC2）、换转换组（规则组/注入组）、换采样时间、
拉长采样窗口 250 倍、开启模拟开关 booster 均无效，软件侧已无可调项。
建议换一块板子，或用外部 ADC 模块（如 I2C 的 ADS1115）绕过片内 ADC。

## SD 卡（SPI 模式）

板载没有 SD 卡座，用 SPI 接口的 MicroSD 模块外接。引脚固定如下：

| SD 模块 | 板子 | MCU | 说明 |
| --- | --- | --- | --- |
| CS / SS / DAT3 | D10 | PD14 | 软件片选，空闲高 |
| MOSI / DI / CMD | D11 | PB5 | SPI1_MOSI, AF5 |
| MISO / DO / DAT0 | D12 | PG9 | SPI1_MISO, AF5 |
| SCK / CLK | D13 | PA5 | SPI1_SCK, AF5 |
| VCC | 5V | — | 模块板载 3.3V 稳压器，必须给 5V |
| GND | GND | — | 与板子共地 |

时钟：初始化 250 kHz（`per_ck/256`），数据传输 1.95 MHz（`PLL1Q/128`）。

### 命令

| 命令 | 作用 |
| --- | --- |
| `sd` / `sd info` | 初始化并打印卡类型、寻址方式、容量 |
| `sd init` | 强制重新初始化（换卡后必须执行） |
| `sd read <lba>` | 读一个 512 字节块，打印 MBR 签名/分区类型和前 16 字节 |
| `sd wtest <lba>` | 写入图案块再读回逐字节比对 |
| `sd pins` | 打印 CS/SCK/MOSI/MISO 的实时电平 |
| `sd jump` | 纯 GPIO 回环：驱动 PB5、读 PG9（验证 D11–D12 是否连通） |
| `sd echo` | SPI 回环：发 `55 aa 01 fe` 并回读（验证 SCK/MOSI/MISO） |
| `sd probe` | 打印一次 CMD0 交换的原始字节 |
| `sd rdprobe <lba>` | 打印 CMD17 之后 12 个原始字节 + CMD13 卡状态 |

### FileX 挂在 SD 上

裸块层通过后，`app_fxsd.c` 用同一套 FileX 核心（`fx_media_*`）把卡当成真实介质，
媒体驱动直接调 `app_sdspi_read_block/write_block`，不需要 LevelX（SD 卡控制器自己
管擦除和磨损）。

| 命令 | 作用 |
| --- | --- |
| `sd mount` | 挂载卡上现有的 FAT 卷（自动解析 MBR 分区偏移） |
| `sd umount` | 卸载卷 |
| `sd ls` | 列根目录 |
| `sd format [sectors]` | 格式化卷，默认 262144 扇区（128 MB），会清空卡上数据 |
| `sd put <name> <text>` | 创建/覆盖文件并写入文本 |
| `sd cat <name>` | 读回文件内容 |
| `sd rm <name>` | 删除文件 |

实测（32 GB SDHC，SPI 1.95 MHz）：

```text
SD mount rc=0  part_lba=64  total_sectors=61067200     ← 卡上原有 FAT32（31.27 GB）
SD put HELLO.TXT hello-from-h563  rc=0
SD f=HELLO.TXT size=15 ; SD DATA text=hello-from-h563
SD format sectors=262144 rc=0  (2.1 s)  free_bytes=134131712  ← 128 MB 测试卷
SD put AFTER.TXT post-format-ok rc=0 → umount → mount → 文件仍在
```

格式化时卷从 0 号扇区开始（superfloppy），**会覆盖卡的分区表**——格式化之后
把卡插回 PC 只会看到一个 128 MB 的卷，需要在 PC 上重新格式化才能恢复 32 GB 分区。

**大容量卷请在有 PC 的地方格式化。** 实测结果：

| 卷大小 | 板上格式化耗时 | 结果 |
| --- | --- | --- |
| 128 MB（262144 扇区） | 2.1 s | 正常 |
| 1 GB（2097152 扇区） | 约 3–4 min | 正常，`free_bytes`≈1 GB，文件读写无误 |
| 32 GB（61067264 扇区） | >15 min | **卷损坏**，根目录读出乱码 |

瓶颈是 SPI 单块写延迟：每个 512 B 块约 10–13 ms（卡内部编程时间占主要部分），
32 GB 卷单是 FAT 表就有约 30 MB（6 万个块），格式化时间不可接受，而且大容量下
FAT/根目录最终没有正确落盘。

但**挂载大容量 FAT32 没有这个问题**：本工程成功挂载过卡上原有的 32 GB FAT32 分区
（`part_lba=64`、`total_sectors=61067200`），文件创建/读写/删除都正常。所以推荐的
用法是：**在 PC 上把卡格式化成 FAT32，板子只负责挂载和读写**。

### 采集落盘到 SD 卡

writer 线程不再绑死模拟盘，两个介质藏在同一张操作表后面（`app_acq.c` 的
`acq_media_ops_t`），`acq target sim|sd` 切换，默认仍是模拟盘（不需要插卡）。

```text
acq target sd            ACQ target=sd rc=0
acq ch 4
acq start dma 50000      ACQ start=ok mode=dma rc=0
（3 秒）
acq stop                 windows=354 samples=708000 files=1
                         drop=0 dma_ovr=0 qdrop=0 flush=57
acq check                ACQ CHECK PASS file=LOG0000.CSV lines=354
                         channels=4 samples=708000 bytes=20776
sd ls                    SD f=LOG0000.CSV size=20776
```

卡上的行长这样（数值是浮空噪声，因为片内 ADC 的外部件坏了，但格式正确）：

```text
0,500,4,518,912,641,547,721,645,419,704,644,351,724,643
 │  │  │  └─ch0─┘ └─ch1─┘ └─ch2─┘ └─ch3─┘
 │  │  └─ 通道数
 │  └─ 每窗帧数
 └─ 窗口序号
```

SD 目标用 1 MB 轮转上限（`APP_ACQ_SD_FILE_MAX_BYTES`），模拟盘仍是 4 KB：
4 KB 在卡上意味着每秒建一个新文件。

顺带验证了持久化：采集中途拔掉 USB 再插回，`sd ls` 里 `LOG0000.CSV`（20776 字节）
依然完整——writer 每批都 flush，已落盘的数据不受掉电影响。

### 把卡写进"只能断电才能恢复"状态的三个原因

接 SD 之后有一段反复翻车：卡会突然对所有命令返回 0xFF，只有拔电才能恢复。
三个原因叠在一起，现在都修了：

1. **`sd` 命令没和 writer 共用互斥锁**。`acq target sd` 之后采集在跑，此时敲
   `sd xxx` 会让 console 和 writer 同时操作 SPI1，命令被切断就把卡写进异常态。
   现在 `console_sd()` 是薄封装，进函数先拿 `app_fxsd_lock()`，和 writer 同一把锁。
2. **写块失败时过早释放 CS**。卡在编程期间若丢掉片选，会一直拉低 DO 并忽略后续
   命令（`sd pins` 会看到 `miso=0` 且内部上拉拉不动）。现在无论 data response
   是否正确，都先等 busy 结束再拉高 CS。
3. **逐字节传输太慢**。512 字节一次一个字节地传，writer 被更高优先级的 sampler
   线程抢占时字节间隔被拉长，卡超时放弃整块。现在整块走一次
   `HAL_SPI_TransmitReceive`，格式化时间也从 2.1 s 降到 1.2 s。

另外 **MISO 必须开内部上拉**：SPI 模式卡空闲时 DO 是高阻，没有上拉就没有确定
电平（空闲读 0 而不是 0xFF）。

### SD 通路是真的（验证方法）

怀疑"文件只是写进了 RAM 缓存"时，用拔卡测试可以一锤定音：

1. `sd ls` → 6 个文件（`LOG0000.CSV` 20776 字节 + `F0..F4.TXT`）
2. **把 TF 卡从卡座里取出** → `sd init rc=-1`、`sd mount rc=-1`、`sd ls count=-1`
   文件列表完全消失
3. **插回卡** → 6 个文件全部恢复
4. `reboot`（软复位清空 RAM 缓存）→ 文件仍在，大小正确，`G.TXT` 读回
   `lasting-9b`，`free_bytes` 正好少 4096（一个簇）

外加裸块层：`sd wtest 5000..5007` 写 8 个扇区后逐字节比对，全部
`mismatches=0`。

### 又一个会丢数据的坑：文件操作后必须 flush 媒体

拔卡插回后发现 `F4.TXT` 从 9 字节变成 **0 字节**。原因是 `sd put` / `sd rm`
只做了 `fx_file_close()`，此时**目录项（含文件大小）还留在 FileX 的媒体缓存里**，
掉电或复位就丢了。

修复：`app_fxsd_write_file()` 和 `app_fxsd_delete()` 结束前都调用
`fx_media_flush(&sd_media)`。采集路径不受影响，因为 writer 每批都 flush。

### 接入 FileX 时踩的坑

1. **分区偏移**：PC 写过的卡 0 号扇区是 MBR，而 FileX 直接读 0 号扇区当引导扇区。
   驱动里要先探测分区表，把第一个分区的 LBA 加到所有逻辑扇区上（本工程探测到 64）。
2. **格式化期间不能探测分区**：格式化时卷在 0 号扇区，如果仍去探测，旧的 MBR 还在，
   偏移会被设成 64，导致引导扇区写在 0 而 FAT/目录写在 64 之后，卷立刻损坏。
3. **CSD v2 容量是 `(C_SIZE+1) × 512 KiB`**，换算成 512 字节块要乘 1024；写成
   `(C_SIZE+1) × 1 MiB` 会得到**两倍**容量（32 GB 卡会被报成 64 GB）。核对方法：
   算出来的总扇区数应当和卡上分区表里的分区大小对得上。
4. **`fx_media_driver_free_sector_update` 必须是 `FX_FALSE`**。设为 `FX_TRUE` 是
   LevelX 那套"驱动自己维护空闲扇区"的语义，让 FileX 误判可用空间，写文件时报
   `FX_SECTOR_INVALID`（0x89）。

### 踩过的两个坑（都会表现为"卡完全无响应"或"数据错位"）

1. **MISO 引脚必须一直是复用模式**。`HAL_SPI_MspInit` 里若把 PG9 先配成 AF、
   再覆盖成普通输入，SPI 外设就收不到 MISO，读数恒为 `0x00`。
2. **数据起始 token `0xFE` 不能用"最高位为 0"的规则去等**。R1/R2 响应是
   "第一个最高位为 0 的字节"，但 `0xFE` 的最高位是 1，用同一套逻辑会把 token
   当填充字节跳过，接着把数据块的第一个字节当成响应，整块数据错位一位。
   正确做法是单独等 `0xFE`（同时识别 `0b0000xxxx` 数据错误 token）。

另外三个环境因素也确认过：CS 必须在每条命令后正确释放、初始化时钟要 ≤400 kHz
（本工程用 250 kHz）、更换卡后必须 `sd init` 重新初始化。

## 模拟前端自检流程

新板子到货后，用这条流程在几分钟内判定 ADC 是否可用。判定阈值来自一块
**已知正常的 STM32F207**（同样方法实测：VREFINT 1495、DAC 回环 2048→2055、
四个通道区间互不重叠）。

### 一条命令

```bash
./scripts/adc_healthcheck.py          # 第 1 阶段，不用接线
./scripts/adc_healthcheck.py --wired  # 接好两根线后，加上第 2 阶段
```

也可以直接在串口敲 `acq hwcheck` 和 `acq hwcheck ext`。

### 第 1 阶段：内部基准 + DAC 回环（无需接线）

| 检查 | 判据 | 依据 |
| --- | --- | --- |
| VREFINT | 1350–1580（反推 VDDA 3.1–3.6 V） | 正常板 1495–1506 |
| 温度传感器 | 350–1150 | 正常板 750–930 |
| VBAT/4 | 500–1000 | 正常板 745–750 |
| DAC 回环 @0 | 0–400 | 同脚 `DAC1_OUT1`→`ADC1_INP18` |
| DAC 回环 @2048 | 1650–2450 | 正常板 2055 |
| DAC 回环 @4095 | 3700–4095 | — |
| 单调性 | 0 < 2048 < 4095 逐级增大 | — |

这一阶段的用意是**区分"转换器坏"和"输入通路坏"**：内部基准不需要经过焊盘，
DAC 回环需要。如果基准对、回环不对，问题就在输入通路；两者都不对则是转换器本身。

### 第 2 阶段：外部电压 + 四通道分离（接 3V3→A1/PC0，GND→A2/PC3）

| 检查 | 判据 |
| --- | --- |
| `PC0`(IN10) 单次采样 | 3700–4095 |
| `PC3`(IN13) 单次采样 | 0–300 |
| 四通道 DMA 采集 ch0 最小值 | ≥3500 |
| 四通道 DMA 采集 ch3 最大值 | ≤600 |
| **两通道区间间隔** | **≥2500 LSB** |

最后一条是关键判据。坏掉的前端会让所有通道都停在中间值附近，此时**每个读数
单看都"合理"**，但区间间隔会接近 0。F207 上实测间隔 ch0_min(4082) −
ch3_max(22) = 4060，余量极大。

### 判定表

| 第 1 阶段 | 第 2 阶段 | 结论 |
| --- | --- | --- |
| PASS | PASS | **前端正常**，可以开始采集开发 |
| PASS | FAIL | 先查接线；接线确认无误则输入通路有问题 |
| FAIL | — | 转换器自身异常（基准或回环不对），基本可判定芯片问题 |

### 已知异常样本

本仓库开发用的那块 H563ZI：

```text
ADC HC begin
ADC HC vrefint    raw=1506 band=1350..1580 PASS
ADC HC vdda_mv=3290
ADC HC temp       raw=750  band=350..1150 PASS
ADC HC vbat       raw=750  band=500..1000 PASS
ADC HC dac0       raw=1499 band=0..400 FAIL          ← 输入通路读不到 0 V
ADC HC dac2048    raw=1496 band=1650..2450 FAIL
ADC HC dac4095    raw=1495 band=3700..4095 FAIL
ADC HC loopback monotonic FAIL (1499, 1496, 1495)    ← 三个码值几乎相同
ADC HC FAIL failures=4
```

基准三项全过、回环四项全失败 —— 这就是"转换器好、输入通路坏"的典型特征。

即"基准全对、输入全错"，正是第 1 阶段要抓的形态。同一套判据在 F207 上全部通过。

## 时间戳与文件头（CSV v2）

日志自带 RTC 日历：文件第一行是文件头，之后每行第一个字段是时间戳。

```
#ACQ2 start=2026-11-22 19:50:47 epoch=848692247 ts=epoch_s rate=10000 ch=1 frame=500
848692247,0,500,1,461,908,518
848692247,1,500,1,437,561,509
```

- 头部以 `#` 开头，说明时间基准（`ts=epoch_s`）、采样率、通道数、每窗帧数；
  解析器自动跳过
- 每行：`epoch,idx,count,nch,min0,max0,avg0[,...]`
- `epoch` 是自 **2000-01-01 00:00:00** 起的秒数（不是 Unix epoch），
  用 `<epoch>` 直接填入任何需要绝对时间的工具即可
- 需要毫秒精度时用 `idx` 换算：`t = epoch_of_idx0 + (idx - idx0) × frame / rate`

### RTC 时钟源

驱动**优先 LSE**（板载 32.768 kHz 晶振，实测可用），失败时回退 **LSI**
（约 32 kHz，漂移按百分比算）。`rtc` 命令会显示实际使用的源。

NUCLEO 板默认没有 VBAT 电池，断电后日历从头开始，上电后设置一次即可：

```
rtc                              # RTC ready=1 source=lse(32.768kHz) time=...
rtc set 2026-11-22 19:50:00      # RTC set rc=0
```

热复位（看门狗/软件复位）不会丢失时间：驱动用 `TAMP->BKP0R` 里的魔数判断
日历已在运行，跳过重新初始化。

## 数据格式

每次触发转换 N 个通道，一个"帧"= 每通道一个样本。窗口固定 **500 帧**，
一行 CSV 记录一个窗口的全部通道统计：

```
epoch,idx,count,nch,min0,max0,avg0[,min1,max1,avg1...]
```

4 通道示例：

```
848692247,281,500,4,422,582,501,425,615,519,438,591,516,452,612,525
```

- `epoch` 时间戳（见上一节），`idx` 窗口序号，`count` 帧数（恒为 500），
  `nch` 通道数（1–4）
- 之后每通道 3 个字段：`min`、`max`、`avg`（12 bit，0–4095）
- `acq check` 逐行校验：字段数必须等于 `4 + 3*nch`、`nch` 与本次采集一致、
  `min ≤ avg ≤ max`、`max ≤ 4095`、`count == 500`，并额外检查
  **时间戳单调不减**与 **idx 连续**，结果行给出 `hdr` / `first` / `last` /
  `span_s` / `bad_ts`

```
ACQ CHECK PASS file=LOG0000.CSV lines=56 channels=4 samples=112000 bytes=3884
ACQ CHECK time hdr=1 first=2026-11-22 19:50:17 last=2026-11-22 19:50:20 span_s=3 bad_ts=0
```

单文件模拟盘 4 KB 轮转（`LOG0000.CSV`…`LOG0003.CSV` 循环覆盖），SD 卡 1 MB 轮转。

## 为什么多通道必须用 DMA

一次触发会把整个 rank 序列连续转换完，每通道约 0.6 µs。EOC 中断从进中断到读 DR
远慢于这个间隔，于是在扫描序列下 EOC 只来得及反映**最后一次转换**，`OVR` 被置位，
其余通道的数据在寄存器里被覆盖。

实测（10 kHz、3 s、中断路径）：1 通道 32,498 次回调，2 通道 32,499 次，4 通道 32,499 次
——无论几路，每次触发只拿到 1 个样本。同一时刻的 DMA 路径是 33,000 / 66,000 / 132,000
样本，严格按 1×/2×/4× 缩放。

所以 `acq start irq` 在通道数 > 1 时直接拒绝：

```
ACQ start=fail mode=irq rc=-8
ACQ ERR multichannel-requires-dma
```

中断路径保留给单通道低速场景（也是单通道基线的回归项）。

## 软件结构

```
ADC 序列扫描（TIM3 每触发一次扫完 N 路）
        │
        ├─ GPDMA1 通道1：单节点循环链表，双半缓冲（半个 = 约 10 ms 的帧）
        │
        └─→ sampler 线程：把就绪半缓冲搬进交织环形缓冲(8192)
                          → 每 500 帧算一次每通道 min/max/avg
                          → 记录入 TX_QUEUE(32 深)，不碰文件系统
                                    │
                             writer 线程：每 50 ms 批量写出 + 一批一次 flush
                                    │
                             FileX → LevelX → RAM 模拟 NOR
```

- 半个 DMA 缓冲按**帧**定尺寸（`rate/100`，64–1024 帧），让半个缓冲正好覆盖
  一个 10 ms 线程 tick。这一步把 4 通道 100 kHz 的 `dma_ovr` 从 47 降到 0。
- 交织环形缓冲必须是 2 的幂（索引用掩码），编译期有两个 static assert 守住
  "2 的幂"和"至少放得下两个 4 通道窗口"。
- `app_fxdisk_lock()` 让 writer 线程与 `fs` 命令族串行访问 FileX。

## 板上已验证

| 项目 | 结果 |
| --- | --- |
| 构建 | `text 179,860 / data 2,396 / bss 141,320`，`.bin` 182,264 B |
| 启动 | `FSD mount=formatted sectors=120 reason=1` → `READY tx-data-logger 1.0.0` |
| 解交织自检 | `acq demux`：4 通道注入 `1000c..1000c+99` 斜坡，每通道 min/max/avg 全部命中，`ACQ DEMUX PASS` |
| 多通道拒绝 | `acq ch 2` + `acq start irq 10000` → `rc=-8` / `multichannel-requires-dma` |
| 2 通道 100 kHz | 3.3 s：`samples=668000 drop=0 dma_ovr=0 qdrop=0`，`acq check PASS channels=2` |
| 4 通道 50 kHz | 4.3 s：`windows=434 samples=868000 drop=0 dma_ovr=0 qdrop=0 flush=86` |
| 4 通道 100 kHz | 3.3 s：`windows=668 samples=1336000 drop=0 dma_ovr=0 qdrop=0`，即 **400 k 样本/秒零丢失** |
| CSV 内容 | `281,500,4,422,582,501,425,615,519,438,591,516,452,612,525` |
| 线程栈 | `sampler=400/2048`、`writer=1544/3072`、`console=1444/4096`、`monitor=1008/2048` |
| 一键自检 | `./scripts/selfcheck.sh all`：`basic` 10、`fs` 19、`acq` 42、`mch` 36、`dma` 20、`rtc` 19、`usb` 9（需接线）、`watchdog` 4、`fault` 5 |
| USB 栈 | `USB ready=1 connected=0`：USBX 初始化成功，未插线时按未连接处理，不影响其余八组 |

## 串口命令

USART3（ST-LINK 虚拟串口，PD8/PD9，115200 8N1）。文件系统命令与
[SimDisk](../NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) 相同，采集部分为：

| 命令 | 作用 |
| --- | --- |
| `acq ch` | 打印当前通道数与上限 |
| `acq ch <1..4>` | 选择通道数（重建 ADC 序列并重新校准；采集中拒绝） |
| `acq start dma [hz]` | 多通道/高速采集，`hz` 1000–100000 |
| `acq start irq [hz]` | 单通道中断路径 |
| `acq stop` | 停止、写空队列、关闭文件 |
| `acq info` | 模式、采样率、`ch`、窗口/样本/文件数、min/max、`drop`/`dma_ovr`/`qdrop`/`flush`，以及 ADC `isr` 计数与 `SQR1/CFGR/ISR` |
| `acq check` | 回读最新日志并按 `nch` 逐通道校验 |
| `acq demux` | 确定性解交织自检（不依赖外部信号） |
| `rtc` | 显示 RTC 状态（时钟源、当前时间、epoch） |
| `rtc set <YYYY-MM-DD HH:MM:SS>` | 设置日历时间（断电后需重设一次） |
| `acq level` | 逐通道打印一次性软件触发采样的原始值/档位，并附 VREFINT、VDD、内部温度与 VBAT |
| `acq level <0-19>` | 单测某个 ADC 通道号（0–19） |
| `acq dac [0-4095]` | 用 `DAC1_OUT1`(PA4) 驱动 `ADC1_INP18` 回环，验证外部通道能否读到电压 |
| `acq selftest` | 采集 1.5 s → 停止 → 校验 |
| `dma env` / `dma test` | GPDMA 通道属性 / 受 TrustZone 影响的安全属性矩阵 |
| `usb` | USB 状态：栈是否就绪、主机是否已配置、CDC 收发字节数与丢弃数 |
| `dump <name>` | 把文件内容从 CDC 端口原样推给主机（`cat /dev/ttyACM1 > f.csv`） |

## 一键自检

```sh
./scripts/selfcheck.sh basic     # 构建 + 符号 + 烧写 + 状态/栈/故障/日志
./scripts/selfcheck.sh fs        # 文件系统 19 项断言
./scripts/selfcheck.sh acq       # 单通道中断 10 kHz + DMA 50/100 kHz，42 项断言
./scripts/selfcheck.sh rtc       # RTC 走时 + 设置/拒绝非法值 + CSV 头与时间戳单调性，19 项断言
./scripts/selfcheck.sh mch       # 解交织 + 中断拒绝 + 2/4 通道 DMA，36 项断言
./scripts/selfcheck.sh dma       # GPDMA 属性矩阵 + 采集自愈，20 项断言
./scripts/selfcheck.sh usb       # USB CDC 枚举 + 命令往返 + dump 导出
./scripts/selfcheck.sh watchdog  # 线程卡死 → IWDG 复位
./scripts/selfcheck.sh fault     # BusFault 记录与重启报告
./scripts/selfcheck.sh all       # 九组顺序执行
```

可用 `PORT=/dev/ttyACM0` 覆盖串口。

`usb` 组需要**用户 USB-C 口接到主机**（不是 ST-LINK 那个）。没插线时该组输出
`SKIP usb: no CDC port found` 并以退出码 2 退出，`all` 会把它记为 SKIP 继续跑；
发布前用 `STRICT=1 ./scripts/selfcheck.sh all` 让"没插线"变成失败。

## USB CDC 命令通道

### 一根线，两个通道

CDC-ACM 让主机把它当成一个串口（Linux `/dev/ttyACM1`、Windows `COMx`、
macOS `/dev/cu.usbmodem*`）。它和 USART3 **并行**存在：

- 输入：两个通道各有独立的行缓冲，但都在 `console` 线程里解析，最终调用同一个
  `console_dispatch()`。所以命令语义完全一致，且不会有两个线程同时进命令处理。
- 输出：`app_log_drain()` 把同一份字节既写 UART 也写 USB，USB 侧是**异步**的
  （进环形缓冲，由 `usb_tx` 线程发出）。主机没接或读得慢时只会丢 USB 的字节，
  UART 不受影响。

### 端点分配

| 端点 | 方向 | 类型 | 用途 |
| --- | --- | --- | --- |
| EP0 | 双向 | Control | 枚举、类请求（`SET_LINE_CODING` 等） |
| 0x81 | IN | Interrupt | 串口状态通知 |
| 0x02 | OUT | Bulk | 主机 → 设备（RX） |
| 0x83 | IN | Bulk | 设备 → 主机（TX） |

描述符是**手写**的（`app_usb.c` 里 85 字节的 `usb_device_framework`），没有引
ST 的 `usb_desc` builder。原因：那个 builder 单个文件 766–1021 行，为了"任意
类组合"做了大量运行时拼装；本项目只有一个类，手写 85 字节既看得懂也改得动，
出问题时可以直接对着 USB 规范逐字段核。设备类在**设备描述符**里声明为 CDC，
只用一个功能所以**不需要 IAD**。

### 为什么读写都要设超时

`ux_device_class_cdc_acm_read()` 默认会在 USBX 内部等到 `UX_NON_CONTROL_TRANSFER_TIMEOUT`
（50 s）。而看门狗按 **1.5 s 无心跳**判定线程卡死——一个空闲但正常的 USB 链路
就会被误判成故障并复位板子。

所以激活回调里用 `SET_READ_TIMEOUT` / `SET_WRITE_TIMEOUT` 把超时压到
100 ms / 200 ms（单位是 tick）：读线程最多阻塞 100 ms 就能回来打心跳，链路空闲
与链路故障因此在看门狗眼里都不再是"卡死"。

### `dump` 的时序

`dump` 用 `app_fxdisk_read_stream()` 一边读文件一边把 256 字节块直接写进 CDC
（阻塞写），**不经过日志环**。为了保证文件字节不被日志插进来打断：

1. `app_usb_flush()` + 2 tick 让已排队的日志先发出去
2. 置 `usb_dump_active`，`app_log_drain()` 的 USB 镜像与 `usb_tx` 线程同时让路
3. 阻塞发送 `DUMP begin ...`、文件字节、然后清标志
4. 结束后 `DUMP ok name=... bytes=N` 走正常日志路径，两个通道都能看到

整个过程持有 `app_fxdisk_lock()`，因此 writer 线程不会在导出途中改动这个文件。

### 资源占用

| 项 | 增量 |
| --- | --- |
| Flash | +37.7 KB（`text` 209,316 → 247,084） |
| RAM | +22.4 KB（`bss` 155,464 → 177,904），其中 12 KB 是 USBX 内存池 |
| 线程 | `usb_rx`（2048 B 栈，含一次性 USBX 初始化）、`usb_tx`（1536 B 栈） |

## 已知限制

- 只有 PC0 接过真实信号；PC1–PC3 的读数是悬空噪声，通道表与排针脚位的对应关系
  需要按板子手册再核一次。
- 通道选择是"表里的前 N 个"，不支持任意组合；要任意组合需要把 `acq_chan_table`
  改成运行时可写，并处理各通道采样时间差异。
- 模拟盘掉电即失；RTC 未接，日志没有绝对时间戳。
- `fs fill` / `fs stress` 是调试命令，量产固件应裁掉。
- USB 只有 CDC-ACM：还没有 MSC 数据盘，导出仍需 `dump` 命令而不是直接拷盘。
- USB 侧输出是尽力而为：主机读得慢会丢 USB 字节（`usb` 命令里的 `tx_drop`），
  UART 不受影响。`dump` 是例外，它用阻塞写，丢了会返回失败。
- `dump` 全程持文件系统锁，导出大文件会让采集 writer 等待；P3-2 换成 MSC 后
  需要先解决这个介质归属问题。
- 4 通道 100 kHz 已零丢失，但不是硬实时保证：sampler 线程仍与 10 ms tick 对齐，
  再提高速率需要更高 tick 频率，或把采集搬出 RTOS 线程。

## 文档

- [report.md](./report.md)：多通道实现与踩坑复盘
- [Application Note](../Application_note.md)：方向、路线与验收标准
- [SimDisk README](../NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md)：单通道基线
