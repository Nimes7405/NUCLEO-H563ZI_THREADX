# ThreadX + TrustZone 模板配置经验

> **准备开始新项目？** 先查看[新项目复制与命名流程](./New_project_naming.md)，
> 按流程复制模板并同步目录名、Makefile 目标名和调试配置。

本文从 USBX HID 移植项目的复盘中提取仅适用于本模板的 RTOS、ThreadX
和 TrustZone 配置要求。USBX HID、USB-PD/CAD、UCPD 附着检测和 USB 线材排查
属于具体应用，不是本模板的基础配置，因此不在本文展开。

> **工具链、250MHz 时钟树、GPIO/MSP、中断、OpenOCD/GDB 调试和裸机 Makefile
> 搭建等板级通用内容请查《NUCLEO-H563ZI 裸机模板 TIPS》**
> （`NUCLEO-H563ZI_TEMPLATE/NUCLEO-H563ZI_TIPS.md`）。本文只写 ThreadX 特有、
> 裸机 TIPS 没有覆盖的部分，两份配合使用。

## 1. 先确认 TrustZone 执行状态，再选 ThreadX 模式

- TrustZone 已启用（例如 OpenOCD 显示 `TZEN`）不等于应用正在 Non-Secure
  状态。应确认 CPU 复位后的实际安全状态，以及启动代码是否执行了
  Secure-to-Non-Secure 跳转。
- 本模板针对已验证的 NUCLEO-H563ZI Secure 启动状态，默认使用
  `THREADX_MODE=SECURE`，对应 `TX_SINGLE_MODE_SECURE=1`。
- Makefile 将安全模式宏用于 ThreadX C 内核和 Cortex-M33 GNU 汇编端口。
  不应只对应用代码设置此宏。
- Secure/Non-Secure 模式不匹配可能导致 ThreadX 第一次上下文切换时出现
  无效异常返回（`CFSR.INVPC`）并进入 HardFault。编译、链接和烧录校验成功
  均不能证明安全状态配置正确。
- `THREADX_MODE=NON_SECURE` 仅切换 ThreadX 编译模式；它不会生成 Secure
  启动程序、SAU/IDAU 配置、Non-Secure 向量表或存储器/外设安全属性。
  只有工程另行实现并验证完整 Secure-to-Non-Secure 启动链后，才可使用。

## 2. ThreadX 端口与启动源必须完整且版本匹配

本模板引用同一 STM32Cube_FW_H5 包内的 ThreadX common 和 Cortex-M33 GNU
port；ThreadX 低层初始化汇编位于本工程 `Core/Src/tx_initialize_low_level.S`。
移植或更换 Cube 固件包时，保持内核、端口和低层初始化来自兼容版本。

ThreadX 端口不只是几个 C 文件。构建必须包含上下文保存/恢复、调度、线程
栈构造、系统返回、计时中断和低层初始化所需的汇编文件。遗漏时可能链接
失败，也可能使调度或首次上下文切换异常。

一个能跑的 ThreadX 工程，Makefile 必须包含以下三类源：

- **内核 C 源码**：`$(THREADX_DIR)/common/src/*.c` 全部（mutex/semaphore/
  queue/event_flags/block_pool/byte_pool/timer/trace 等），本模板直接
  `wildcard` 引入。
- **Cortex-M33 GNU 端口汇编**（`$(THREADX_DIR)/ports/cortex_m33/gnu/src/`）：
  `tx_misra.S`、`tx_thread_context_restore.S`、`tx_thread_context_save.S`、
  `tx_thread_interrupt_control.S`、`tx_thread_interrupt_disable.S`、
  `tx_thread_interrupt_restore.S`、`tx_thread_schedule.S`、
  `tx_thread_stack_build.S`、`tx_thread_system_return.S`、
  `tx_timer_interrupt.S`。
- **本工程低层初始化**：`Core/Src/tx_initialize_low_level.S`（已按 250MHz
  定制）。不要再加 Cube 端口目录里自带的 `tx_initialize_low_level.S`，
  它的时钟常量与本工程主频不一致。

端口目录里的 `tx_thread_secure_stack_*.{c,S}` 用于 TrustZone 双模式，
`TX_SINGLE_MODE_SECURE=1` 下**不要**加入构建。

端口汇编拥有 ThreadX 的关键异常处理函数：低层初始化提供 ThreadX 的
`SysTick_Handler`，Cortex-M33 端口提供 `PendSV_Handler` 和
`SVC_Handler`。不要再在 `stm32h5xx_it.c` 或应用中定义同名处理函数。

## 3. ThreadX 和 HAL 使用不同硬件时基

- 本模板的 ThreadX tick 为 **100 ticks/s**，由 ThreadX 端口使用 SysTick。
- HAL 的 **1 ms tick** 通过 TIM6 驱动。HAL 的时间基文件、TIM6 MSP 时钟使能、
  NVIC 配置和 `TIM6_IRQHandler` 必须一起加入构建。
- 不要让 HAL 和 ThreadX 同时配置或接管 SysTick，否则会导致 tick 计数、
  延时或调度异常。
- ThreadX 低层初始化中的 `SYSTEM_CLOCK`/`SYSTICK_CYCLES`、`tx_user.h` 中的
  `TX_TIMER_TICKS_PER_SECOND`、主时钟配置和应用中的 `tx_thread_sleep()`
  换算必须一致。若改变 CPU 主频或 ThreadX tick 频率，应一并更新并验证。
  换算公式为 `SYSTICK_CYCLES = SYSTEM_CLOCK / TX_TIMER_TICKS_PER_SECOND - 1`；
  本模板即 `250000000 / 100 - 1 = 2499999`（`SYSTEM_CLOCK=250000000` 在
  `tx_initialize_low_level.S`，`TX_TIMER_TICKS_PER_SECOND=100` 在 `tx_user.h`）。

## 4. 内存策略要与应用和链接布局一致

- 本模板的示例线程控制块和栈是静态分配的；没有创建动态 ThreadX byte pool
  或启用动态 ThreadX heap 的要求。
- 若应用改为动态分配，需显式规划 ThreadX 可用内存并核对链接脚本、RAM
  区域、安全属性和启动时传入的未使用内存边界。
- 添加静态线程、队列、栈或其他中间件对象时，确认链接脚本中的 RAM 空间
  足够；不要因为已有静态对象就假设需要启用动态 heap。
- 在 `tx_application_define()` 或线程入口中创建/使用 ThreadX 对象。HAL、
  GPIO 等板级初始化应在进入 `tx_kernel_enter()` 前完成，避免在内核定义
  回调中依赖尚未启动的调度行为。

## 5. 变更配置后干净构建，并分层验证运行

更改 `THREADX_MODE`、ThreadX 用户配置、端口宏、低层初始化时钟常量或汇编
端口后，执行干净构建，避免旧对象文件保留先前的编译定义：

```sh
make clean
make -j4
```

建议按以下顺序验证：

1. 编译与链接通过，检查输出中包含 ThreadX 低层初始化、SysTick、PendSV、
   `tx_application_define`、应用入口和所需 HAL MSP/IRQ 符号。
2. 烧录后先确认没有在第一次线程切换处 HardFault；LED 心跳可作为示例应用
   线程已运行的外部迹象。
3. 调试时检查 `CFSR`/`HFSR` 和异常栈帧。若有 `INVPC`，优先核对实际 Secure/
   Non-Secure 执行状态、ThreadX 模式宏和对应的端口/启动链。
4. 检查 ThreadX tick 和线程状态。tick 增长只证明内核计时路径在运行，不
   单独证明目标应用线程已就绪或正在执行。
5. 若后续集成 USB、通信或其他外设，再沿各自的初始化和事件链检查；不要
   把外设未启动直接归因于 ThreadX 调度。

运行时观测（可替代/补充断点调试）：

- **串口**：示例线程用 `printf` 经 ST-LINK 虚拟串口（USART3，115200 8-N-1）
  输出 `** ThreadXXX : ... **`，是最直接的运行证据。读取命令：

  ```sh
  stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb
  cat /dev/ttyACM0            # Ctrl+C 退出
  ```

- **OpenOCD 读内核 tick**：先 `arm-none-eabi-nm <TARGET>.elf | grep _tx_timer_tick`
  查地址，再暂停目标读两次，递增说明 SysTick 计时路径在跑：

  ```sh
  openocd -s . -f openocd/st_nucleo_h5.cfg \
    -c "init" -c "halt" \
    -c "echo [format 0x%08x [mrw 0x2000XXXX]]"
  ```

  注意：tick 递增只证明内核计时在跑；要确认目标线程已就绪/在执行，还需
  结合串口输出或线程状态（`tx_thread_state`）一起看。

## 6. 本模板的配置位置

| 配置内容 | 模板位置 |
| --- | --- |
| ThreadX 安全模式选择、内核/端口源列表 | `Makefile` |
| ThreadX tick 频率和内核配置 | `Core/Inc/tx_user.h` |
| ThreadX SysTick 及低层上下文初始化 | `Core/Src/tx_initialize_low_level.S` |
| ThreadX 线程与静态栈示例 | `Core/Src/app_threadx.c` |
| HAL TIM6 时基 | `Core/Src/stm32h5xx_hal_timebase_tim.c` |
| TIM6 MSP 与 IRQ | `Core/Src/stm32h5xx_hal_msp.c`、`Core/Src/stm32h5xx_it.c` |
| 主时钟、GPIO 初始化及内核入口 | `Core/Src/main.c` |
| Flash/RAM 布局 | `STM32H563ZITX_FLASH.ld` |

本指南记录的是模板的通用 ThreadX/TrustZone 约束；项目级原始移植过程和
USBX 专项经验仍保留在 USBX HID 工程的 `report.md` 与
`NUCLEO-H563ZI_TIPS.md` 中。

## 7. 可靠性层新增经验（Tx-Reliability-Monitor）

1. **OpenOCD 必须用 AP1 的官方 target**：`interface/stlink-dap.cfg` +
   `transport select dapdirect_swd` + `source [find target/stm32h5x.cfg]`。
   模板自带的 HLA/AP0 配置会在 examination 阶段失败。
2. **模板需要补 HAL tick 回调**：`TIM6_IRQHandler` 里只调用
   `HAL_TIM_IRQHandler()` 不会自动递增 tick，必须在 `main.c` 实现
   `HAL_TIM_PeriodElapsedCallback()` 调用 `HAL_IncTick()`。
3. **IWDG 需要 `LSI_STARTUP_TIME`**：在 `stm32h5xx_hal_conf.h` 定义
   （本工程用 130 UL），否则 `stm32h5xx_hal_iwdg.c` 编译失败。
4. **ThreadX 会把线程栈填成 0xEF**：`tx_thread_create` 在
   `TX_DISABLE_STACK_FILLING` 未定义时用 `TX_STACK_FILL`（0xEF）覆盖整段栈；
   水位扫描和 canary 必须用 0xEF，不能用 0xA5。
5. **带 `vsnprintf` 的线程栈至少 2 KB**：768 B/1 KB 会触发 UsageFault
   `STKOF`；用 `stack` 命令看实测水位。
6. **`READY` 要在能收命令之后再打印**：放在 console 线程启动后，而不是
   `tx_application_define`；否则主机连发命令时单字节 RX 会溢出。
7. **调试时冻结 IWDG**：`DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP`。
8. **故障记录放 `.noinit`**：链接脚本加 `NOLOAD` 段，能跨复位、不能跨掉电；
   启动时读取一次并清除 `valid`。

详细定位过程见本工程 [report.md](./report.md)。

## 8. 控制节点新增经验（Tx-Control-Node）

1. **引脚**：TIM1_CH1 = **PE9 = CN10-4**；ADC1_INP10 = **PC0 = Arduino A1 /
   CN9-3（Morpho CN4-38）**。
2. **ADC 同步**：TIM1 `TIM_TRGO_UPDATE` 触发 ADC1 注入组；JEOC 回调只保存
   原始值，控制放在 TIM1 更新 ISR，采样与控制解耦。
3. **控制频率**：TIM1 20 kHz 更新，每 2 次执行一次控制 = 10 kHz；比直接
   10 kHz PWM 更容易保持整数关系。
4. **前馈 + PI**：输出有 0..1000 硬限幅时，纯 PI 在大阶跃下容易饱和；
   前馈 `sp * 1000 / 4095` 先把输出放到稳态附近，PI 只修正残差。
5. **编译宏不要和枚举同名**：`-DAPP_PLANT_ADC=0` 会把枚举
   `APP_PLANT_ADC` 替换成 `0`；用 `APP_PLANT_ADC_DEFAULT`。
6. **H5 ADC 采样时间档位**没有 64.5 cycles，用 24.5/47.5/92.5 等。
7. **主栈按中断嵌套核算**：TIM1 + ADC + UART + FPU 扩展帧，建议 ≥2 KB。
8. **切换构建变体后先清串口缓冲**，否则会先看到上一版固件的输出。

详细过程见本工程 [report.md](./report.md)。

## 9. P3-lite 新增经验（MPU / guard / WWDG）

1. **H5 的 MPU 用 CMSIS 接口**：`MPU->TYPE.DREGION` 在本板为 12；用
   `mpu_armv8.h` 的 `ARM_MPU_SetRegion()` / `ARM_MPU_Enable()` 直接写
   RBAR/RLAR，比 HAL 的 `MPU_Region_InitTypeDef`（VOS 字段语义不同）更省事。
2. **静态分区**：Flash `0x0800_0000–0x081F_FFFF` RO/X；RAM
   `0x2000_0000–0x2009_FFFF` RW/XN；外设 `0x4000_0000–0x5FFF_FFFF`
   device/XN。先覆盖大块，再用高优先级 region 切出 guard。
3. **guard 取 32 B**：MPU 最小粒度 32 B，够覆盖一次越界写；每个线程栈 block
   改成 `{uint32_t guard[8]; UCHAR stack[SIZE];}`，并让 stack 32 B 对齐。
4. **guard 要可归因**：故障记录保存 `guard_id`，MemManage 时用 `MMFAR` 反查
   线程名；`CFSR=0x82`（MMARVALID + DACCVIOL）是“写只读 guard”的典型值。
5. **安全模式要有出口**：连续 3 次故障才降级，`STATUS safe=1`、控制输出关；
   必须提供 `fault clear`，否则一次注入就把设备永久锁在安全模式。
6. **WWDG 的 window 是“允许刷新的计数区间”**，不是“最短等待时间”：
   counter `0x7F`、window `0x60` 时，计数在 `0x7F..0x61` 刷新会立即复位；
   必须等降到 `<=0x60` 再刷新。初始化后立刻刷新是“启动即复位循环”的根因。
7. **WWDG 初始化顺序**：`__HAL_RCC_WWDG_CLK_ENABLE()` 后调
   `HAL_WWDG_Init()`；HAL 先写 CR（启动计数）再写 CFR（prescaler/window），
   所以初始化返回时计数已经在跑，首次刷新时机必须算好。
8. **首次刷新用 80 ms 门控**：本工程 PCLK1 250 MHz、prescaler 128，
   tick = 2.097 ms；`0x7F-0x60=31 tick ≈ 65 ms` 后才允许刷新。monitor 线程
   启动 WWDG 后先 `tx_thread_sleep(80 ms)`，再进入 200 ms 轮询；
   `APP_WWDG_FIRST_FEED_MS=60` 是二次保险。
9. **WWDG 与 IWDG 分工**：WWDG（≈134 ms）抓“monitor 还活着但主循环卡死”；
   IWDG（4 s）抓“monitor 自身或调度整体卡死”。`stall 3` 停控制心跳 →
   IWDG；`stall 4` 停 WWDG 刷新 → WWDG。
10. **自检前先 `fault clear`**：guard/crash 会把 `sequence` 累加到安全模式；
    basic/control/mpu 等模式开头清一次，避免上一次注入影响下一次断言。
11. **`fault clear` 要清缓存**：除了 `.noinit` 的 `fault_record`，还要清
    RAM 里的 `last_fault` / `have_last_fault`，否则命令打印 cleared、报告仍是旧值。

## 10. P4 新增经验（release profile / 校准）

1. **用 `APP_PROFILE` 管构建档位**：`release` 固定 `-O2` 并定义
   `APP_RELEASE=1`；不要在命令行同时传 `OPT`，否则会覆盖 release 的优化级别。
2. **破坏性命令用编译期裁掉**，不要只写“量产别敲”；同时让 `HELP` 不列出，
   再用 `release_check.sh` 断言 `guard/stall/crash/reboot` 不在 HELP 中。
3. **BOOT/READY/STATUS 都带 profile**：现场不用猜当前烧的是 dev 还是 release；
   日志解析器对新增字段保持前缀匹配即可。
4. **校准默认值必须等价于旧行为**：`offset=0`、`scale=1`，否则 P3 的
   控制自检会被校准代码悄悄改变。
5. **切换校准要重置滤波器/积分**：ADC 反馈滤波和 PI 积分里可能残留旧状态，
   切换 offset/scale 后先重置，再让闭环重新收敛。
6. **校准值先做运行时**：先把 `cal show/set/reset` 和默认路径验证通，再做
   Flash/EEPROM 掉电保持；不要一开始把写 Flash 和闭环调参混在一起。

## 11. 片内 Flash 参数页经验（P4 校准掉电保存）

1. **H5 的最后 8 KB（`0x081FE000–0x081FFFFF`）是 EDATA 区**，不是普通 Flash：
   普通 `FLASH_TYPEPROGRAM_QUADWORD` 编程会失败、擦除报告 OK 但内容不变、
   调试器读回 `0x00000000`。参数页要放在**前一个普通扇区**
   （本工程用 `0x080FE000`，bank 1 sector 127）。
2. **Secure 单镜像必须 `-mcmse`**。不加时 `__ARM_FEATURE_CMSE` 未定义，
   HAL 的 `IS_FLASH_SECURE_OPERATION()` 恒假 → 写 `NSCR/NSKEYR` 操作安全 Flash，
   结果是 `PGSERR`(0x40000)。加上 `-mcmse` 后 HAL 自动改用 `SECCR/SECKEYR`。
3. **MPU 只读 Flash 会拦编程**：HAL 的 quad-word 编程是 4 次 CPU store，
   会被 P3 的 Flash RO region 判成 `MemManage`(`CFSR=0x82`, `MMFAR=flash 地址`)。
   编程前后临时 `ARM_MPU_Disable()`/`ARM_MPU_Enable(PRIVDEFENA)`，并确保
   这段已经关中断（本工程在关中断窗口内完成擦+写+校验）。
4. **擦除会阻塞取指**：8 KB 扇区擦除实测让 20 kHz ISR 停顿约 **2.0 ms**；
   单条 16 B 编程只要 0.1–0.2 ms。设计上用「追加式日志」把擦除频率降到
   512 次保存一次。
5. **不要用 `%f`**：`--specs=nosys.specs` 默认 nano printf 不支持浮点，
   要么加 `-u _printf_float`（体积代价大），要么按整数缩放打印
   （本工程用 `offset=%ld.%02ld`）。
6. **写 Flash 后重置健康统计**：一次性停顿会污染 `ovr`/`jit` 指标，
   擦写完成后清空统计并打印 `pause_ms`，否则自检的 `ovr==0` 断言会误报。
7. **链接脚本要预留参数区**：把 `FLASH` 长度从 2048K 收到 2032K、加 `PARAM`
   段并导出 `_param_page_start` 符号，防止以后代码变大覆盖参数页。
