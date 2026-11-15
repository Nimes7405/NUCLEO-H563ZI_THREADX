# NUCLEO-H563ZI ThreadX Tx-Data-Logger 复盘

本工程从 `Tx-FileX-SimDisk`（标签 `v1.0.0-single-channel`）派生，只改采集这一层：
把"单通道、每触发一次转换"升级为"1–4 通道、每触发扫完整序列"。
文件系统、可靠性层、GPDMA 诊断工具（`dma env` / `dma test`）原样继承，不再重复说明。

## 1. 设计

### 1.1 数据通路

```
TIM3 TRGO ──► ADC1 规则组序列（rank1..rankN）
                   │  每次触发产生 N 个转换结果
                   ▼
          GPDMA1 通道1 循环链表，双半缓冲
                   │  半个 = rate/100 帧（约 10 ms）
                   ▼
        sampler 线程：交织样本 → 环形缓冲(8192) → 每 500 帧算每通道 min/max/avg
                   │  TX_QUEUE(32)
                   ▼
        writer 线程：每 50 ms 批量写 CSV + 一批一次 fx_media_flush
                   ▼
              FileX → LevelX → RAM 模拟 NOR
```

沿用单通道阶段的结论：采样线程不碰文件系统，落盘线程批处理；两者用
`app_fxdisk_lock()` 与 `fs` 命令族互斥。

### 1.2 通道表

`acq_chan_table[]` 固定四路，都是 GPIOC 上的连续引脚（`ADC1_INP10..INP13`）。
`acq ch N` 取表中前 N 项，重建 ADC 并重新校准。之所以不做任意组合：
不同通道的采样时间要求不同（内部通道要几百个周期），任意组合需要一套
"每通道采样时间"的配置语法，当前收益不大。

### 1.3 记录格式

一帧 = 每通道一个样本；一行 CSV = 一个 500 帧窗口的全部通道统计：

```
idx,count,nch,min0,max0,avg0[,min1,max1,avg1...]
```

把 `nch` 写进每一行，是为了让文件离开固件也能自解释：`acq check` 不需要
知道"当时配了几路"，按行里的 `nch` 校验字段数与一致性即可。

## 2. 踩到的问题

### 2.1 rank 常量是"寄存器偏移"，不是 1/2/3/4

第一次配置多通道时写的是：

```c
channel.Rank = ADC_REGULAR_RANK_1 + i;   /* 错 */
```

`ADC_REGULAR_RANK_x` 实际是 `LL_ADC_REG_RANK_x = (SQR1 偏移 | 字段位偏移)`，
不能做算术。`+i` 之后 LL 的 `LL_ADC_REG_SetSequencerRanks()` 匹配不到任何 rank，
后几路根本没写进去。用 `acq ch` 打印 `SQR1` 才看出来：

| 期望（4 通道） | 实际 |
| --- | --- |
| `L=3, SQ1=10, SQ2=11, SQ3=12, SQ4=13` | `L=3, SQ1=10, SQ2=1, SQ3=0, SQ4=0` |

改成查表：

```c
static const uint32_t ranks[APP_ACQ_CH_MAX] = {
    ADC_REGULAR_RANK_1, ADC_REGULAR_RANK_2, ADC_REGULAR_RANK_3, ADC_REGULAR_RANK_4};
```

之后 `SQR1=0x0D30B283`：`L=3`、`SQ1=10`、`SQ2=11`、`SQ3=12`、`SQ4=13`，与期望一致。
**教训**：HAL 里那些带 `REGOFFSET` 的"常量"只能原样传回，不能参与运算。

### 2.2 环形缓冲必须保持 2 的幂

把环从 1024 改成 `CH_MAX * WINDOW_FRAMES * 4 = 8000` 后，`acq demux` 直接失败
（`frames got=0`）。原因：环形缓冲用 `& (SIZE - 1)` 取模，8000 不是 2 的幂，
索引和"待取样本数"全错。

现在固定 8192，并加了两个编译期断言，防止以后改窗口/通道数时踩回去：

```c
typedef char acq_ring_pow2_check[(ACQ_RING_SAMPLES & (ACQ_RING_SAMPLES - 1U)) == 0U ? 1 : -1];
typedef char acq_ring_fit_check[(ACQ_RING_SAMPLES >= (2U * APP_ACQ_CH_MAX * APP_ACQ_WINDOW_FRAMES)) ? 1 : -1];
```

### 2.3 DMA 半缓冲要按"帧"定尺寸，而不是固定样本数

最初半个缓冲固定 512 **样本**。单通道时是 512 帧，没问题；4 通道时只有 128 帧，
50 kHz 下一个半缓冲 2.56 ms，而 sampler 线程 10 ms 才醒一次——两个半缓冲位根本
不够用，`dma_ovr` 飙到 619。

改成按帧定尺寸 `frames = rate/100`（64–1024，即约 10 ms），并实测对比：

| 半缓冲策略 | 4ch@100 kHz `dma_ovr` |
| --- | --- |
| 固定 512 样本（128 帧，5.12 ms × 2 个半缓冲/tick） | 47 |
| 按帧 10 ms（1000 帧，1 个半缓冲/tick） | **0** |

"一个半缓冲正好一个 tick"是这里的关键：两个半缓冲填满一个 tick 时余量为 0，
tick 稍有抖动就丢整块。

### 2.4 EOC 中断跟不上扫描序列（所以多通道只能 DMA）

配置正确后，中断路径仍然只拿到 1 个样本/触发。加计数验证：

| 路径 | 1 通道 | 2 通道 | 4 通道 |
| --- | --- | --- | --- |
| EOC 中断（10 kHz，3 s） | 32,498 | 32,499 | 32,499 |
| GPDMA（10 kHz，3.3 s） | 33,000 | 66,000 | 132,000 |

DMA 严格 1×/2×/4× 缩放，中断路径却恒定等于触发次数。原因是一次触发会把整个序列
连续转换完（每通道约 0.6 µs），远快于"进中断 → HAL 处理 → 读 DR"的耗时；
扫描序列下 EOC 只能反映最后一次转换，`ADC_ISR` 的 `OVR` 位被置起。

因此多通道在中断路径上是**结构性**不可行，不是调参问题。现在 `acq start irq`
在通道数 > 1 时返回 `-8` 并打印 `ACQ ERR multichannel-requires-dma`。

### 2.5 HAL_ADC_DeInit 会解引用 Instance

`acq_adc_config()` 会先 `HAL_ADC_DeInit()` 再重新初始化，以便切换通道数。
但 `app_acq_init()` 里第一次调用时 `hadc1` 还是全零，`HAL_ADC_DeInit()` 直接读
`Instance->CR`（地址 0x8），上电即 `PRECISERR`、`BFAR=0x8`，复位后反复重启。

修法是显式判空：

```c
if (hadc1.Instance != NULL) { (void)HAL_ADC_DeInit(&hadc1); }
```

## 3. 解交织自检（`acq demux`）

多通道最容易错的是"样本归错通道"，而这种错误在悬空引脚上看不出来（四路都是噪声）。
所以加了一个不依赖外部信号的确定性检查：往环形缓冲里塞 4 路交织斜坡
（通道 c 的值恒在 `1000c .. 1000c+99` 之间循环），走**真实**的窗口统计路径，
再逐通道核对 min/max/avg：

```
ACQ DEMUX ch=0 min=ok max=ok avg=ok (0/99/49)
ACQ DEMUX ch=1 min=ok max=ok avg=ok (1000/1099/1049)
ACQ DEMUX ch=2 min=ok max=ok avg=ok (2000/2099/2049)
ACQ DEMUX ch=3 min=ok max=ok avg=ok (3000/3099/3049)
ACQ DEMUX PASS channels=4 failures=0
```

这样即使没有传感器，也能证明解交织、窗口统计和 CSV 字段顺序都是对的。

## 4. 实测

| 配置 | 时长 | 样本（帧×通道） | ring drop | dma_ovr | qdrop | 回读校验 |
| --- | --- | --- | --- | --- | --- | --- |
| 1ch, irq 10 kHz | 9 s | 90,000 | 0 | – | 0 | PASS |
| 1ch, dma 100 kHz | 3 s | 320,500 | 0 | 0 | 0 | PASS |
| 2ch, dma 100 kHz | 3.3 s | 668,000 | 0 | 0 | 0 | PASS channels=2 |
| 4ch, dma 50 kHz | 4.3 s | 868,000 | 0 | 0 | 0 | PASS channels=4 |
| 4ch, dma 100 kHz | 3.3 s | 1,336,000 | 0 | 0 | 0 | PASS channels=4 |

4 通道 100 kHz 即 400 k 样本/秒，零丢失；固件 `text 179,860 / bss 141,320`。

## 5. 下一步

1. 把 `acq_chan_table` 改成可配置（任意通道组合 + 每通道采样时间），并从命令行动态加通道。
2. 接真实介质（SPI NOR / SD）：只换 `app_fxdisk.c` 的底层驱动，上层与自检不动。
3. 日志加 RTC 时间戳与文件头（起始时间、采样率、通道表），让 CSV 脱离固件可解读。
4. 8 小时长跑（多通道 100 kHz）验证稳定性与栈水位。
