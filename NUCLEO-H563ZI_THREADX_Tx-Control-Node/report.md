# NUCLEO-H563ZI ThreadX Tx-Control-Node 复盘

## 目标

在 `Tx-Reliability-Monitor` 的可靠性层之上实现路线 1：TIM1 PWM + ADC 同步采样
+ 10 kHz 控制环，并同时支持：

- 默认**软件一阶被控对象**（无需接线即可验证控制环）；
- `PLANT_ADC=1` / `plant adc` 切换到 **PWM → RC → ADC 外部回环**。

## 结果摘要

- 目录 `NUCLEO-H563ZI_THREADX_Tx-Control-Node` 从可靠性层复制，`TARGET`、
  `.vscode/launch.json`、启动横幅与 `READY` 已同步。
- P4 dev `-O0`：`text 124,272 / data 2,344 / bss 21,720`，`.bin` 126,620 B。
- P4 dev `-O2`：`text 97,020 / data 2,344 / bss 21,696`，`.bin` 99,364 B。
- P4 release `-O2`：`text 95,844 / data 2,344 / bss 21,696`，`.bin` 98,188 B。
- Flash 参数页（`app_param.c` + HAL flash 驱动）约占 `-O0` text 6.2 KB；
  release 比 dev O2 小 1.2 KB（裁掉破坏性命令的收益）。
- TIM1 更新中断 20 kHz，控制环 10 kHz；`cnt` 每 500 ms 增加 10,000。
- 20 kHz 周期抖动 min/max 约 `49.94/50.06`、`48.42/51.54`、`49.96/50.06` µs，
  spread 0.1–3.2 µs，`ovr=0`。
- 阶跃 `set 1000` → `fb=1000 out=245 err=0`；`set 3000` → `fb=3000 out=733 err=0`。
- `stall 3` → `WATCHDOG stall=control no-heartbeat=1500ms` → `BOOT reset=iwdg fault=none`。
- `stall 4` → `BOOT reset=wwdg fault=none`，实测 94–131 ms（理论 134 ms）。
- `guard` → `CFSR=0x00000082`、`MMFAR=<guard>`、`FAULT guard=prod_a`；
  连续 3 次故障进入 safe mode，`fault clear` + `ctrl on` 恢复。
- `PLANT_ADC=1` 烧写后 `CTRL mode=adc`，ADC 原始值随输入变化。
- `./scripts/selfcheck.sh all` 七组全部 PASS（basic/control/mpu/param/wwdg/watchdog/fault）。
- 1 h O0 soak PASS（3603 s）：`boots/faults/resets=0`、`safe/drop=0`、
  `ovr=0`、worst jitter spread 3.49 µs；`uptime` 增量 3,600,500 ms，
  `wdg_feed` 568→7,769，控制 `cnt` 增量 72,010,000。
- P4：`cal` 参数校准、`APP_PROFILE=release`（O2、裁掉破坏性命令）、
  `OPT=-O2 selfcheck all` 七组 PASS、`release_check.sh` PASS。
- O2 dev 15 min soak PASS：`boots/faults/resets=0`、`ovr=0`、
  worst jitter spread 1.63 µs；`uptime +900,090 ms`。
- P4 追加：`cal` 校准值掉电保存在片内 Flash 参数页（`0x080FE000`），
  重启/重新烧写后自动恢复；`cal 500 1.0` → `fb=adc-500`、`cal 0 0.5` → `fb=adc/2`
  实测吻合；`selfcheck all` 扩为七组（新增 `param`）。
- P2 阶段 `-O0` 固件连续运行约 15 min（控制 ISR `cnt=18,179,997` @20 kHz）无复位；P3-lite 已扩展到 1 h soak。

## 实现内容

### PWM 与 ADC

- TIM1：`PSC=0`、`ARR=12499`，边沿对齐，250 MHz / 12500 = **20 kHz**。
- `TIM_TRGO_UPDATE` 作为 ADC 注入组的外部触发；ADC1 注入通道 10（PC0）。
- `HAL_ADCEx_Calibration_Start()` 后 `HAL_ADCEx_InjectedStart_IT()`；JEOC 回调
  把原始值写入 `ctrl_adc_raw`。
- PWM 引脚 PE9（CN10-4），ADC 引脚 PC0（Arduino A1 / CN9-3）。

### 控制环

- TIM1 更新 ISR 20 kHz；每 2 次执行一次控制 → **10 kHz**。
- 控制器 = 前馈 + PI：
  - 前馈 `ff = sp * 1000 / 4095`，把输出直接放到稳态附近，避免大阶跃时
    输出长时间饱和；
  - `Kp=0.05`、`Ki=0.002`，积分带抗饱和；
  - 输出 0..1000 permille，映射到 CCR1 0..12499。
- 软件被控对象：`fb += (target - fb) * 0.02`（tau ≈ 5 ms）。
- ADC 模式：`fb` 对 `ctrl_adc_raw` 做 1/4 一阶滤波。
- 关闭控制环（`ctrl off`）时输出 0、积分清零。

### 抖动与状态

- 控制 ISR 用 `DWT->CYCCNT` 测 20 kHz 周期，记录 min/max。
- 周期偏离期望 12500 周期超过 ±50% 计 `ovr`。
- `CTRL` 行每 500 ms 由 monitor 线程输出：
  `mode/on/sp/fb/adc/out/err/jit_us/ovr/cnt/kp/ki`。
- 控制 ISR 递增 `APP_HB_CONTROL`，纳入 IWDG 心跳；`stall 3` 可模拟该心跳失效。

### 命令

在继承的 console 上新增：

- `set <0..4095>`
- `plant <sim|adc>`
- `ctrl <on|off>`
- `gain <kp> <ki>`

## 构建与链接验证

- 关键符号：`TIM1_UP_IRQHandler`、`ADC1_IRQHandler`、
  `HAL_ADCEx_InjectedConvCpltCallback`、`app_control_tim_isr`、
  `app_control_report`、`app_control_start`。
- `HAL_ADC` / `HAL_ADCEx` 源已加入 Makefile；`HAL_ADC_MODULE_ENABLED` 已打开。
- `PLANT_ADC` 作为 Makefile 变量传入 `-DAPP_PLANT_ADC_DEFAULT=$(PLANT_ADC)`。
- 主栈从 1 KB 提升到 2 KB，容纳 TIM1/ADC/UART 中断嵌套。

## 板上验证记录

### 控制环阶跃

```text
CTRL mode=sim on=1 sp=1000 fb=1000 adc=588 out=245 err=0 jit_us=49.44/50.54 ovr=0 cnt=329997 kp=0.05 ki=0.002
SET sp=3000
CTRL mode=sim on=1 sp=3000 fb=3000 adc=647 out=732 err=0 jit_us=49.34/50.67 ovr=0 cnt=349997 kp=0.05 ki=0.002
CTRL mode=sim on=1 sp=3000 fb=3000 adc=611 out=733 err=0 jit_us=48.65/51.32 ovr=0 cnt=359997 kp=0.05 ki=0.002
```

### 控制心跳看门狗

```text
STALL control simulated; IWDG should reset in ~4 s
WATCHDOG stall=control no-heartbeat=1500ms; stop feeding IWDG
BOOT reset=iwdg fault=none
```

### ADC 默认变体

```text
make clean && make PLANT_ADC=1
make flash PLANT_ADC=1
...
BOOT reset=software fault=none
CTRL mode=adc on=1 sp=0 fb=451 adc=447 out=0 err=-451 jit_us=48.42/51.54 ovr=0 cnt=9997 kp=0.05 ki=0.002
```

### 自检

```text
basic    9 项 PASS
control 10 项 PASS（INFO: sp=3000 fb=3000 out=733 jit=49.82/50.15us ovr=0；cal show/set/reset）
mpu      5 项 PASS（CFSR=0x82，MMFAR=guard，FAULT guard=prod_a）
wwdg     3 项 PASS（BOOT reset=wwdg，fault=none）
watchdog 4 项 PASS（stall=control，reset=iwdg）
fault    5 项 PASS（BusFault，BFAR 记录）
```

### 1 小时长跑实测

```text
SOAK PASS seconds=3603.2 status=840 boots=0 faults=0 resets=0 safe=0 drops=0
  uptime_ms=3884490 wdg_feed=7769 hb=38845,10699,388450,7769,7769,77689739
  ctrl cnt=77681539 ovr=0 err=0 fb=3000 worst_jit_spread=3.49us
  stack console=1152/3072 logger=524/1024 monitor=1240/2048 prod_a=952/2048 prod_b=920/2048
```

基线 `uptime_ms=283,990`、`wdg_feed=568`、`cnt=5,671,539`；增量分别为
3,600,500 ms（≈60.0 min）、7,201、72,010,000。栈最高为 monitor
1240/2048（60.5%），其余 ≤ 56%；控制反馈稳定在 `fb=3000`、`err=0`。

### 长跑脚本

`scripts/soak.py` 不重启、不注入故障，只做运行期断言：

- 自动 `fault clear` → `plant sim` → `ctrl on` → `set 3000`；
- 周期读取 `STATUS`/`CTRL`/`STACK`，检查无 `BOOT`/`WATCHDOG`/`FAULT seq=`；
- `reset` 不变、`safe=0`、`drop=0`、`rx_drop=0`；
- `uptime`/`wdg_feed`/`hb`/控制 `cnt` 单调增长，`ovr=0`，抖动 spread ≤ 5 µs；
- 各线程栈使用率 ≤ 80%，结束再确认 `fault none`。

先跑 60 s 冒烟，再跑 1 h；1 h 是功能验收的最低长跑档，后续再逐步加长。

## P3-lite 容错实现

### 静态 MPU

- 使用 CMSIS `mpu_armv8.h`（H5 的 `MPU->TYPE.DREGION=12`），不用 HAL
  `MPU_Region_InitTypeDef`（字段语义不同）：
  - Flash `0x0800_0000–0x081F_FFFF`：RO + 可执行；
  - RAM `0x2000_0000–0x2009_FFFF`：RW + 不可执行；
  - 外设 `0x4000_0000–0x5FFF_FFFF`：device + 不可执行；
  - 5 个线程栈 guard：32 B，RO + 不可执行。
- 每个线程栈 block 为 `guard[8]` + `stack[SIZE]`（32 B 对齐）；
  `app_stacks` 登记 guard 地址、大小和线程名，`app_mpu` 统一写 region。
- 效果：栈溢出写穿到 guard 立即 MemManage，而不是静默踩坏相邻线程数据；
  `MMFAR` + `guard_id` 直接给出线程名。

### 故障恢复 / 安全模式

- `.noinit` 故障记录增加 `guard_id`；MemManage 时若 `MMFAR` 落在某个 guard，
  记录对应线程名。
- `sequence >= 3` 且 magic 有效 → safe mode：`STATUS safe=1`，控制环强制
  `on=0`、PWM 输出 0，避免带故障继续驱动功率级。
- `fault clear` 清零 `valid/sequence/safe`；之后 `ctrl on` 恢复。
- 单次故障仍按原流程记录并复位，保留现场；连续故障才降级，避免偶发干扰
  直接锁死设备。

### 第二级 WWDG

- 参数：PCLK1 250 MHz、prescaler 128、counter `0x7F`、window `0x60`。
  计数频率 `250 MHz / 4096 / 128 = 476.8 Hz`（2.097 ms/tick），
  超时 `64 × 2.097 ≈ 134 ms`；刷新窗口为计数 `0x60..0x40`。
- 启动流程：monitor 线程启动 WWDG 后 `tx_thread_sleep(8)`（80 ms），
  越过 window 下界（`0x7F-0x60=31 tick ≈ 65 ms`）再首次刷新；之后
  200 ms 周期刷新。若上电立刻刷新（计数仍 > window）会触发 WWDG 复位，
  这是最初“启动即复位循环”的根因。
- `stall 4` 停止刷新，实测复位延迟 94–131 ms（取决于命令落点相对计数器
  相位），`BOOT reset=wwdg fault=none`；WWDG 之后仍能正常重启并稳定运行。
- 分工：WWDG 抓“monitor 活着但主循环卡死”；IWDG（4 s）抓“monitor 自身
  或调度整体卡死”。

## P4 release / 参数校准实现

### Release profile

- Makefile 增加 `APP_PROFILE`：`release` 固定 `-O2` 并定义 `APP_RELEASE=1`；
  `make release` = `clean` + `APP_PROFILE=release`。
- `app_config.h` 统一导出 `APP_PROFILE_NAME`（`release`/`dev`）；
  `BOOT`/`READY`/`STATUS` 都带 profile 字段，板上可直接确认跑的是哪版。
- `APP_RELEASE=1` 时 `HELP` 不再列出 `guard/stall/crash/reboot`，对应命令
  和 `console_stall` 编译裁掉；`fault clear`、`cal` 仍保留，便于现场维护。
- 新增 `scripts/release_check.sh`：release 构建、符号/尺寸检查、烧写、
  release 断言（无破坏性命令、profile=release、cal 可用）、control 自检、
  60 s soak。

### 参数校准

- `app_control` 增加 ADC 反馈校准：
  `fb_raw = (adc_raw - offset) * scale`，再限制到 0..4095，
  默认 `offset=0`、`scale=1` 与 P3 行为完全一致。
- `cal show` / `cal set <offset> <scale>` / `cal reset`；
  offset 限制 ±4095，scale 限制 0.01..10.0。
- 修改校准时重置 ADC 滤波器状态和积分，避免切换瞬间的旧反馈造成输出跳变。
- 当前参数在 RAM，复位恢复默认；Flash 掉电保持留到 P4 后续增量。

### 验证

- `OPT=-O2 ./scripts/selfcheck.sh all`：七组 PASS（含 `param`），control 抖动
  `49.86/50.16 µs`、`ovr=0`，`cal` 三项 PASS。
- `./scripts/release_check.sh`：release 构建 95,844 B text / 98,188 B bin；
  `profile=release`，`HELP` 无 `guard/stall/crash/reboot`，
  control 与 60 s soak 均 PASS（release 60 s worst jitter spread 2.13 µs）。
- P4 最终 dev 固件 15 min soak：`SOAK PASS seconds=903.2 status=270
  boots=0 faults=0 resets=0 safe=0 drops=0`；`uptime_ms=907,920`、
  `wdg_feed=1,815`、`ctrl cnt=18,151,411`、`ovr=0`、worst jitter spread 3.63 µs。
- `-O2` dev 15 min soak：`SOAK PASS seconds=903.2 status=271
  boots=0 faults=0 resets=0 safe=0 drops=0`；基线 `uptime=369,990 ms`、
  `wdg_feed=740`、`cnt=7,391,539`，增量分别 900,090 ms、1,800、18,010,000；
  `ctrl cnt=25,401,539`、`ovr=0`、`err=0`、`fb=3000`、worst jitter 1.63 µs；
  栈最高 monitor 1112/2048（54.3%）、console 1200/3072（39.1%）。

### 校准值存储（片内 Flash 参数页）

- 位置：`0x080FE000`（bank 1 sector 127），链接脚本把最后 16 KB 划为
  `PARAM` 段，`FLASH` 长度从 2048K 收到 2032K，防止代码落进参数区。
- 格式：16 B 记录 `magic("CAL0") + offset + scale + CRC32`，正好是 H5 的
  quad-word 编程粒度。
- 策略：**追加式日志**。每次保存找一个空槽写一条，512 槽写满才擦一次扇区；
  掉电导致的半写记录 CRC 校验失败被跳过，上一条仍然有效。
- 启动：`app_control_init()` 里先 `app_param_load()`，读到有效记录立刻套用，
  保证控制 ISR 第一次执行就用正确校准。
- 观测：`PARAM addr=0x080fe000 used=n/512 slot=m erases=e bank=b err=0`。

实测：

```text
PARAM saved offset=250.00 scale=1.500 slot=0 pause_ms=0
CAL offset=250.00 scale=1.500
CAL stored=1/512 slot=1 erases=0 err=0
# 重启后
BOOT reset=software fault=none safe=0 profile=dev
PARAM addr=0x080fe000 used=1/512 slot=1 erases=0 bank=0 err=0
CAL offset=250.00 scale=1.500
```

### 参数存储踩的四个坑

1. **H5 最后 8 KB 是 EDATA 区**。最初的实现放在 `0x081FE000`：擦除返回 OK
   但内容不变、编程失败、OpenOCD 读出来是 `0x00000000`。EDATA 需要 EDATA
   专用的半字/字命令，不是普通 quad-word。改到 `0x080FE000`（bank 1 最后
   一个普通扇区）后一切正常。
2. **TZEN 打开时 HAL 会走错寄存器组**。没有 `-mcmse` 时
   `__ARM_FEATURE_CMSE` 未定义，`IS_FLASH_SECURE_OPERATION()` 恒为假，
   HAL 写 `NSCR/NSKEYR` 去操作安全 Flash，擦除返回 `PGSERR`(0x40000)。
   解决办法是给整个工程加 `-mcmse`（单镜像 Secure 的正确编译方式）。
3. **P3 的只读 Flash MPU region 会拦住编程**。HAL 的
   `FLASH_Program_QuadWord()` 是四条 `*dest_addr = *src_addr`，属于普通
   CPU store，被 `CFSR=0x82 / MMARVALID+DACCVIOL` 打回 MemManage。
   现在在**关中断窗口内**临时关 MPU，编程完立刻恢复。
4. **擦除会阻塞取指**。8 KB 扇区擦除实测让 20 kHz ISR 停顿 **≈2.0 ms**
   （`jit_max=2042.69`、`ovr=+4`）。因为擦除只在 512 次保存后发生一次，
   且是人为操作，处理方式是擦后清空抖动/`ovr` 统计并打印
   `CTRL stats reset after flash op`，同时把 `pause_ms` 打出来留证。

## 问题定位与修复

### 1. `APP_PLANT_ADC` 宏与枚举重名

- **现象**：编译报 `expected identifier before numeric constant`。
- **根因**：Makefile 传入 `-DAPP_PLANT_ADC=0`，而 `app_control.h` 里也有
  枚举常量 `APP_PLANT_ADC`，预处理后变成 `0 = 1`。
- **修复**：编译宏改名 `APP_PLANT_ADC_DEFAULT`，枚举仍为 `APP_PLANT_ADC`。

### 2. ADC 采样时间宏不存在

- **现象**：`ADC_SAMPLETIME_64CYCLES_5` undeclared。
- **根因**：H5 HAL 的采样时间档位是 2.5/6.5/12.5/24.5/47.5/92.5/247.5/640.5。
- **修复**：改用 `ADC_SAMPLETIME_24CYCLES_5`（62.5 MHz 下约 0.39 µs，远小于 50 µs 周期）。

### 3. 大阶跃时纯 PI 容易饱和

- **现象/风险**：`Kp` 较大时，`set 0→3000` 会让输出长时间顶到 1000，
  被控对象冲过设定值，出现 bang-bang。
- **修复**：加入前馈 `ff = sp * 1000 / 4095`，PI 只负责修正残差；实测
  `sp=1000/3000` 都无超调、无持续振荡。

### 4. 切换构建变体时串口有旧固件残留

- **现象**：烧写 `PLANT_ADC=1` 后第一次抓串口，先看到 `mode=sim`，随后才是
  新的 `BOOT` 和 `mode=adc`。
- **原因**：tty 缓冲里还有上一版固件的输出。
- **处理**：自检脚本先 `drain_quiet()`，只认重启后的新 `BOOT` 行；手工验证时
  先清空缓冲再判断。

### 5. WWDG 启动即复位循环

- **现象**：启用 WWDG 后板子每秒多次 `BOOT reset=wwdg`，串口只看到连续
  复位横幅。
- **根因**：WWDG 的 window 不是“最小等待时间”而是“允许刷新的计数区间”；
  `HAL_WWDG_Init` 先写 CR 启动计数、再写 CFR 配置 window，初始化后立刻
  刷新时计数仍高于 window 上界，直接触发复位。另外 HAL tick 若未递增，
  按 tick 计算的首次刷新时机会永远不满足。
- **修复**：`window=0x60`；WWDG 由 monitor 线程在调度启动后初始化；
  用 `tx_time_get()` 做 80 ms 门控，确保首次刷新落在 `<=0x60` 的窗口内；
  之后固定 200 ms 刷新。
- **验证**：`stall 4` 稳定复位，重启后 `wdg_feed` 正常递增、无复位循环。

### 6. `fault clear` 未清除 RAM 缓存

- **现象**：`fault clear` 打印 `FAULT cleared`，但紧接着 `fault` 仍显示
  上一次的 `seq/cfsr/mmfar/guard`。
- **根因**：`app_fault_clear()` 只清了 `.noinit` 的 `fault_record` 和
  `safe_mode`，没有清 `last_fault` / `have_last_fault` 这两个 RAM 缓存；
  `fault` 命令读的是缓存。
- **修复**：`app_fault_clear()` 同时把 `last_fault` 清零、`have_last_fault=0`。
  这样 `fault clear` 后 `fault` 返回 `FAULT none`，长跑脚本和自检才有一致语义。

### 7. 抖动统计口径

- `jit_us` 输出的是 20 kHz **周期**的 min/max，不是控制周期 100 µs；
  控制环每 2 次更新执行一次。自检用 `max-min <= 5 µs` 判定。
- 报告函数的短临界区会让 min/max 偶尔出现 ~3 µs spread，属正常。

## 可复用经验

1. **TIM1_CH1 = PE9 = CN10-4**；**ADC1_INP10 = PC0 = Arduino A1 / CN9-3**。
2. 用 TIM1 `TRGO_UPDATE` 触发 ADC 注入组，JEOC 回调只存值，控制放 TIM 更新 ISR，
   采样与控制解耦。
3. 20 kHz ISR + `/2` 控制比直接 10 kHz PWM 更容易保持控制周期整数关系。
4. **前馈 + PI** 比纯 PI 更适合输出有硬限幅的场合。
5. 编译宏不要和枚举常量同名；用 `_DEFAULT` 后缀区分。
6. 主栈要按"中断嵌套 + FPU 扩展帧"重新核算，本工程从 1 KB 提到 2 KB。
7. 更换构建变体后先清串口缓冲，再断言启动行为。
8. H5 的 MPU 用 CMSIS `ARM_MPU_*` 更直接；先读 `MPU->TYPE.DREGION`
   再决定 region 数量，guard 取 32 B 即可覆盖最小粒度。
9. WWDG 的 window 是“允许刷新的计数区间”，不是“最短等待”；初始化后
   必须等计数器降到 window 内再首次刷新。
10. 故障恢复要有“连续 N 次”阈值和显式 `fault clear`，否则一次调试注入
    就会把设备永久留在安全模式。
11. `clear` 类命令要同时清持久记录和 RAM 缓存；只清 `.noinit` 会出现
    “已清除但报告仍旧”的假象。
12. 量产 release 用编译期裁命令，不要靠“文档说明别敲”；`HELP` 同步隐藏，
    并用 `release_check.sh` 断言命令确实不存在。
13. 校准参数要有默认值（offset=0/scale=1），并在切换时重置滤波器/积分，
    否则默认路径也会被校准代码改变行为。
14. **H5 的 Flash 末尾 8 KB 是 EDATA**，不是普通用户区；参数页要往前挪一个扇区。
15. **Secure 单镜像必须 `-mcmse`**，否则 HAL 的 flash 驱动用 NS 寄存器，
    表现为 `PGSERR` 而不是权限错误。
16. **MPU 只读区会拦 flash 编程**（HAL 用 CPU store 写数据），要在编程那一刻
    关 MPU，并且已经处在关中断窗口内。
17. **参数写 Flash ≠ 免费**：扇区擦除约 2 ms 取指停顿，控制环会丢几个周期，
    要显式记录停顿并把抖动统计窗口重置，否则健康指标被一次性操作污染。
