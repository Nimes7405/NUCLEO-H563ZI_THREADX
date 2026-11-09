# NUCLEO-H563ZI_THREADX_Tx-Control-Node

路线 1：**实时控制节点**（TIM PWM + ADC + 10 kHz 控制环）+ **P3-lite 容错** + **P4 release/校准**。

本工程从
[`Tx-Reliability-Monitor`](../NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)
派生，继承 IWDG + 线程心跳、故障记录、复位原因、环形日志、USART3 状态台、
栈水位和一键自检；业务部分实现 TIM1 PWM、ADC 同步采样和 PI + 前馈控制环。
P3-lite 增加静态 MPU、5 个线程栈 guard、故障归因、安全模式和第二级 WWDG；
P4 增加 release profile（O2、裁掉破坏性命令）、`cal` ADC 参数校准，并把校准值
掉电保存在片内 Flash 参数页（无需外部 Flash / SD）。

## 板上已验证（实测）

| 项目 | 结果 |
| --- | --- |
| 构建 dev `-O0` | `text 124,272 / data 2,344 / bss 21,720`，`.bin` 126,620 B |
| 构建 dev `-O2` | `text 97,020 / data 2,344 / bss 21,696`，`.bin` 99,364 B |
| 构建 release `-O2` | `text 95,844 / data 2,344 / bss 21,696`，`.bin` 98,188 B |
| PWM / 控制率 | TIM1 20 kHz 更新中断，控制每 2 次执行一次 = **10 kHz**；`cnt` 每 500 ms 增加 10,000 |
| 周期抖动 | `jit_us=49.94/50.06`、`48.42/51.54`、`49.96/50.06`（20 kHz 周期 min/max），spread 0.1–3.2 µs，`ovr=0` |
| 1 小时长跑（O0） | `soak.py 3600 s` PASS：`uptime +3,600,500 ms`、`wdg_feed 568→7,769`、控制 `cnt +72,010,000`（20 kHz）、`ovr=0`、worst jitter spread 3.49 µs；无 `BOOT`/故障/复位 |
| 15 分钟长跑（O2） | `soak.py 900 s` PASS：`uptime +900,090 ms`、`wdg_feed 740→2,540`、`cnt +18,010,000`、`ovr=0`、worst jitter spread 1.63 µs；无 `BOOT`/故障/复位 |
| 15 分钟长跑（P4 dev） | `soak.py 900 s` PASS：`boots/faults/resets=0`、`safe/drop=0`、`ovr=0`、worst jitter spread 3.63 µs |
| 阶跃响应 | `set 1000` → `fb=1000 out=245 err=0`；`set 3000` → `fb=3000 out=733 err=0`，500 ms 内稳定 |
| 看门狗（IWDG） | `stall 3` → `WATCHDOG stall=control no-heartbeat=1500ms` → `BOOT reset=iwdg fault=none` |
| 看门狗（WWDG） | `stall 4` → 停止 WWDG 刷新 → `BOOT reset=wwdg fault=none`；实测 94–131 ms（理论 134 ms） |
| MPU / guard | `MPU regions=12 guards=5 enabled`；`guard` → `CFSR=0x00000082`、`MMFAR=<guard>`、`FAULT guard=prod_a` |
| 安全模式 | 连续 3 次故障 → `safe=1`、`CTRL on=0`；`fault clear` + `ctrl on` 恢复 |
| 参数校准 | `cal show/set/reset/erase`；ADC 反馈按 `(raw-offset)*scale` 映射，默认 0/1；实测 `cal 500 1.0` 使 `fb=adc-500`、`cal 0 0.5` 使 `fb=adc/2` |
| 参数掉电保存 | 片内 Flash 参数页 `0x080FE000`（bank 1 sector 127，预留 16 KB）；`cal set` → `PARAM saved ... slot=0 pause_ms=0`，重启后 `CAL offset=250.00 scale=1.500` 原样恢复 |
| 重新烧写保持 | 烧写整个固件后 `CAL offset=777.00 scale=0.500` 仍在（`used=7/512`） |
| Flash 写入开销 | 单条 record 编程 <1 ms（`pause_ms=0`，抖动峰值 ~87 µs，`ovr=0`）；512 次后整扇区擦除一次，实测停顿 **≈2.0 ms** |
| Release profile | `make release`：O2 + `APP_RELEASE=1`，`HELP` 中移除 `guard/stall/crash`（`reboot` 保留）；`release_check.sh` PASS |
| 自检套件 | `basic` 9、`control` 10、`mpu` 5、`param` 8、`wwdg` 3、`watchdog` 4、`fault` 5 全部 PASS |
| O2 自检 | `OPT=-O2 ./scripts/selfcheck.sh all` 七组全通过；控制抖动 `49.86/50.16 µs`、`ovr=0` |

## 引脚与接线

| 信号 | MCU 引脚 | 连接器 | 说明 |
| --- | --- | --- | --- |
| TIM1_CH1 PWM | PE9 | CN10-4 | 默认 0% 占空比，`set` 后输出 |
| ADC1_INP10 | PC0 | Arduino A1 / CN9-3（Morpho CN4-38） | 反馈输入 |

默认 `PLANT_ADC=0`，控制环使用**软件一阶被控对象**，无需任何接线即可验证
10 kHz 控制环、PI 和抖动。

要跑真实模拟回环：

```text
PE9 (CN10-4) ──[ 10 kΩ ]── PC0 (A1/CN9-3)
                              │
                          [ 100 nF ]
                              │
                             GND
```

然后 `plant adc`（或 `make PLANT_ADC=1` 让 ADC 成为默认反馈）。

## 串口命令

USART3（ST-LINK 虚拟串口，115200 8N1）：

| 命令 | 作用 |
| --- | --- |
| `status` / `stack` / `fault` / `wdg` | 继承自可靠性层 |
| `set <0..4095>` | 设定值（ADC counts） |
| `plant <sim\|adc>` | 切换软件被控对象 / 真实 ADC 反馈 |
| `ctrl <on\|off>` | 使能/关闭控制环（关闭时输出 0） |
| `gain <kp> <ki>` | 在线调整 PI 增益（默认 0.05 / 0.002） |
| `cal [show\|set <offset> <scale>\|reset\|erase]` | ADC 反馈校准，写入 Flash 参数页；默认 offset=0、scale=1 |
| `stall <0\|1\|2\|3\|4>` | 3 = 模拟控制心跳卡死（IWDG）；4 = 停止 WWDG 刷新（破坏性） |
| `guard` | 写 prod_a 的 32 B MPU guard，验证 MemManage 归因（破坏性） |
| `fault clear` | 清除故障记录/连续计数/安全模式 |
| `crash` / `reboot` | 故障注入 / 软件复位（破坏性） |

`CTRL` 状态行每 500 ms 输出一次，例如：

```text
CTRL mode=sim on=1 sp=3000 fb=3000 adc=618 out=733 err=0 jit_us=49.94/50.06 ovr=0 cnt=399997 kp=0.05 ki=0.002
```

`jit_us` 是 TIM1 更新中断周期（20 kHz）的 min/max；`cnt` 是更新中断计数。

## 构建与烧写

```sh
make clean
make -j4
make flash
```

ADC 默认反馈：

```sh
make clean && make PLANT_ADC=1
make flash PLANT_ADC=1
```

`-O2` 基线：

```sh
make clean && make OPT=-O2
```

Release 构建（O2 + `APP_RELEASE=1`，裁掉破坏性命令）：

```sh
make release                 # 只构建
make release-flash           # 构建并烧写
# 或
make clean && make APP_PROFILE=release
```

## 一键自检

```sh
./scripts/selfcheck.sh basic     # 继承的可靠性层检查
./scripts/selfcheck.sh control   # 阶跃响应、稳态误差、抖动、ovr、cal
./scripts/selfcheck.sh mpu       # MPU guard → MemManage → CFSR/MMFAR/线程名
./scripts/selfcheck.sh param     # cal 写入 Flash → 重启 → 恢复校验
./scripts/selfcheck.sh wwdg      # stall 4 → WWDG 复位
./scripts/selfcheck.sh watchdog  # stall 3 → IWDG 复位
./scripts/selfcheck.sh fault     # BusFault 记录与重启报告
./scripts/selfcheck.sh all       # 七组顺序执行
```

可用 `PORT=/dev/ttyACM0` 覆盖串口。

## 1 小时长跑

`selfcheck` 是短时破坏性验证；长跑用独立脚本，不重启、不注入故障：

```sh
# 先做 60 s 冒烟
python3 scripts/soak.py --seconds 60 --interval 5 --stack-interval 20

# 正式 1 小时，CSV 便于画 uptime/心跳/控制计数曲线
python3 scripts/soak.py --seconds 3600 --interval 30 --stack-interval 300 \
  --csv /tmp/soak-1h.csv
```

`soak.py` 自动完成：

- `fault clear` → `plant sim` → `ctrl on` → `set 3000`，并确认控制已使能；
- 周期性读取 `STATUS`/`CTRL`/`STACK`，只认运行期间的新行；
- 断言：无 `BOOT`/`WATCHDOG`/`FAULT seq=`，`reset` 不变，`safe=0`，
  `drop=0`，`rx_drop=0`，`uptime`/`wdg_feed`/`hb`/控制 `cnt` 单调增长，
  `ovr=0`，20 kHz 周期抖动 spread ≤ 5 µs，各线程栈使用率 ≤ 80%；
- 结束时再校验 `fault none`，输出 `SOAK PASS/FAIL` 摘要。

1 小时是**功能验收的最低长跑档**；通过后再按需加长到 8/24 h，而不是一上来
就跑 72 h。

## 实现说明

- **PWM**：TIM1 边沿对齐，PSC=0、ARR=12499 → 20 kHz；CCR1 预装载。
- **采样**：ADC1 注入组通道 10（PC0），由 TIM1 TRGO（update）触发；JEOC
  中断把原始值写入 `ctrl_adc_raw`。
- **控制环**：TIM1 更新中断 20 kHz，每 2 次执行一次控制（10 kHz）。控制器为
  **前馈 + PI**，输出 0..1000 permille，带积分抗饱和；软件被控对象是一阶
  `tau = 5 ms`，ADC 模式对反馈做 1/4 一阶滤波。
- **抖动**：控制 ISR 用 DWT->CYCCNT 测量 20 kHz 周期，状态台输出 min/max；
  超过 ±50% 周期计一次 `ovr`。
- **心跳**：控制 ISR 递增 `APP_HB_CONTROL`；`stall 3` 只停止心跳、不停止 ISR，
  用于验证"控制环还在跑但监护逻辑失效"的场景。
- **主栈**：为容纳 TIM1/ADC/UART 中断嵌套，`_Min_Stack_Size` 从 1 KB 提升到 2 KB。

## P4 release / 参数校准

### Release profile

`APP_PROFILE=release` 固定 `-O2`，并定义 `APP_RELEASE=1`：

- `HELP` 不再列出 `guard`、`stall`、`crash`，对应命令编译裁掉；
  `reboot` 属于现场维护命令，两个 profile 都保留；
- `BOOT`/`READY`/`STATUS` 增加 `profile=release` / `profile=dev`；
- `scripts/release_check.sh`：build → flash → release 断言 → control → param → 60 s soak。

### 参数校准

`cal` 用于真实 ADC 反馈的零点/增益校准：

```text
cal show
cal set 100 1.0     # fb = (adc_raw - 100) * 1.0，并限制到 0..4095
cal reset           # 恢复 offset=0、scale=1
cal erase           # 擦除整个参数页（回到出厂默认）
```

### 校准值掉电保存（片内 Flash）

不需要外部 Flash 或 SD：用**片内 Flash 的最后 16 KB** 做参数页。

| 项 | 值 |
| --- | --- |
| 参数页地址 | `0x080FE000`（bank 1 sector 127） |
| 预留范围 | `0x080FC000`–`0x080FFFFF`（16 KB，链接脚本 `PARAM` 段） |
| 记录格式 | 16 B = `magic + offset + scale + CRC32`，正好一个 quad-word |
| 存储方式 | 追加式日志，512 条/扇区，写满才整扇区擦除一次 |
| 掉电安全 | 半写记录 CRC 失败被跳过，上一条仍有效 |
| 写一条 | 约 0.1–0.2 ms 停顿（`pause_ms=0`） |
| 擦一次 | **≈2.0 ms** 停顿（512 次保存才发生一次） |

启动顺序：`app_control_init()` → `app_param_load()` → 有效记录则套用校准；
`tx_application_define()` 打印 `PARAM addr=0x080fe000 used=n/512 ...`。

实现要点（踩过的坑都在 [report.md](./report.md)）：

- **不要用 `0x081FE000`**：那是 H5 的 8 KB EDATA 区，普通 quad-word 擦写无效；
- TrustZone 打开时必须用 `-mcmse` 编译，否则 HAL 写 `NSCR` 而非 `SECCR`，
  擦除返回 `PGSERR`；
- HAL 编程是**CPU store** 到 flash 地址，会被 P3 的 Flash 只读 MPU region 拦成
  `MemManage`，所以编程那一刻关 MPU（已在关中断窗口内）；
- 擦除会阻塞取指约 2 ms，因此 `cal` 操作后清空抖动/`ovr` 统计并打印
  `CTRL stats reset after flash op`，避免把一次性停顿算成控制健康问题。

### O2 验证

- `OPT=-O2 ./scripts/selfcheck.sh all` 七组 PASS（control 抖动 `49.86/50.16 µs`）；
- `./scripts/release_check.sh` PASS（release 60 s soak，jit spread 2.13 µs）；
- O2 dev **15 min soak PASS**：`boots/faults/resets=0`、`safe/drop=0`、
  `ovr=0`、worst jitter spread 1.63 µs；`uptime +900,090 ms`。

## P3-lite 容错

### 静态 MPU 与栈 guard

- 12 个 MPU region：Flash `0x0800_0000–0x081F_FFFF` RO/X；RAM
  `0x2000_0000–0x2009_FFFF` RW/XN；外设 `0x4000_0000–0x5FFF_FFFF` device/XN；
  其余 5 个 region 给线程栈 guard（32 B，RO/XN）。
- 每个线程栈改为 `{uint32_t guard[8]; UCHAR stack[SIZE] aligned(32);}`，
  `app_stacks` 在 `tx_thread_create` 时登记 guard 地址与线程名。
- `guard` 命令故意写 prod_a 的 guard：触发 MemManage，`CFSR=0x00000082`
  （MMARVALID + DACCVIOL），`MMFAR` 指向 guard，故障记录给出
  `FAULT guard=prod_a`，复位原因 `reset=software`。

### 故障恢复 / 安全模式

- 故障记录保存在 `.noinit`，含 `sequence`、`guard_id`、`CFSR/HFSR/MMFAR/BFAR`。
- 连续 3 次故障（`sequence >= 3`）且记录有效 → 进入 safe mode：
  `STATUS safe=1`，控制环强制关闭（`CTRL on=0`，PWM 输出 0）。
- `fault clear` 清零记录/连续计数/安全模式；恢复控制用 `fault clear` + `ctrl on`。
- 单次 guard/crash 故障仍按原流程复位并保留记录，便于定位；只有连续故障才降级。

### 分级看门狗

- **IWDG**：4 s，监控 5 路心跳（monitor/control/logger/console/prod_a）；
  任一心跳在 1.5 s 内未更新则停止喂狗 → `reset=iwdg`。
- **WWDG**：PCLK1 250 MHz、prescaler 128、counter `0x7F`，超时
  `64 × 2.097 ms ≈ 134 ms`；window `0x60`，刷新必须落在计数
  `0x60..0x40` 窗口内。monitor 线程启动 WWDG 后先等 80 ms（越过 window
  下界）再首次刷新，之后每 200 ms 刷新一次。
- `stall 4` 停止 WWDG 刷新，实测 94–131 ms 复位（取决于发命令时计数器相位），
  `BOOT reset=wwdg fault=none`。WWDG 负责“monitor 还活着但主循环卡死”，
  IWDG 负责“monitor 自身卡死”，两者互补。

## 已知限制

- v1 使用 **PE9 单通道 PWM**；PE8（TIM1_CH1N）可后续用于互补输出和死区，
  当前未配置。
- `plant adc` 需要外部 RC 接线；悬空时 ADC 读到噪声，闭环会饱和。
- 软件被控对象是台架模型，不等同于真实电机/功率级；接功率硬件前要重新
  整定增益和保护。
- 单镜像 Secure；MPU 是**静态**分区 + 栈 guard，不含 ThreadX 动态 per-thread
  MPU 隔离，也不含低功耗 tickless。
- `stall` / `crash` / `guard` 是实验室破坏性命令，`make release` 已裁掉。
- 参数页擦除会让 CPU 停顿约 2 ms（每 512 次保存一次）；如果控制环不能容忍，
  需要改成双页轮换并在擦除前主动 `ctrl off`。

## 文档

- [Application Note](../Application_note.md)
- [Tx-Reliability-Monitor](../NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)
- [NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX_TIPS.md)
- [report.md](./report.md)
