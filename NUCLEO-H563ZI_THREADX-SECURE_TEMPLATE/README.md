# ThreadX Secure Makefile template for NUCLEO-H563ZI

This is a reusable, hand-written Makefile project skeleton for STM32H563ZI
applications using HAL and ThreadX. It follows the layout of
[`NUCLEO-H563ZI_TEMPLATE`](../../NUCLEO-H563ZI_TEMPLATE/) while keeping the
Cube HAL/CMSIS and ThreadX implementation sources as external dependencies.
The example starts ThreadX and blinks LED3 (PG4); USBX and USB-PD are not
included.

Before starting an application, copy this directory and synchronize the new
directory name, Makefile `TARGET`, and VS Code ELF path using
[`New_project_naming.md`](./New_project_naming.md).

## Project layout

```text
Core/
  Inc/       HAL, ThreadX and application configuration headers
  Src/       main, application threads, MSP, IRQ, HAL TIM6 tick,
             syscalls, CMSIS system setup and ThreadX low-level init
startup/     STM32H563 startup and vector table
openocd/     Board-specific OpenOCD config and STM32H5 target config
svd/         STM32H563 SVD for Cortex-Debug peripheral views
Makefile     HAL/ThreadX source selection, build, flash and clean targets
*.ld         Secure single-image Flash/RAM layout
.vscode/     Build, clean, flash and Cortex-Debug launch configuration
```

## External dependencies

The template does not vendor the STM32 HAL, CMSIS device headers, or ThreadX
kernel/port implementation. `CUBE_FW` points to the STM32Cube_FW_H5 package;
the local startup file, `system_stm32h5xx.c`, and ThreadX low-level
initialization assembly are included because they are project-level startup
configuration. Override the default paths when building on another machine:

```sh
make clean
make CUBE_FW=/path/to/STM32Cube_FW_H5_V1.7.0
make flash CUBE_FW=/path/to/STM32Cube_FW_H5_V1.7.0
```

`TOOLCHAIN_PATH` and `OPENOCD` can also be overridden. `make` creates ELF,
binary, map, and object/dependency files; `make clean` removes generated
outputs.

## TrustZone mode

`THREADX_MODE` defaults to `SECURE`, matching the NUCLEO-H563ZI setup verified
for this template (TrustZone enabled and execution starts in Secure state).
The selected define is used when compiling both ThreadX C sources and the
Cortex-M33 GNU assembly port:

```sh
make clean && make THREADX_MODE=SECURE
```

`THREADX_MODE=NON_SECURE` only selects the ThreadX single-mode compile define.
It does not create a Secure boot image, SAU/IDAU configuration, security
attribution, or Non-Secure vector table. Use it only with a separately
configured Secure-to-Non-Secure startup and matching memory/peripheral
permissions. A mode mismatch can cause `INVPC` and a HardFault on the first
ThreadX context switch.

For the reusable configuration rules, port/time-base constraints, and
debugging checklist extracted from the USBX HID project's RTOS bring-up, see
[`NUCLEO-H563ZI_THREADX_TIPS.md`](./NUCLEO-H563ZI_THREADX_TIPS.md). It
intentionally excludes USBX/USB-PD application-specific setup.

## Kernel and interrupt setup

- `main.c` configures the 250 MHz board clock and GPIO before calling
  `tx_kernel_enter()`.
- `app_threadx.c` creates a statically allocated thread and stack in
  `tx_application_define()`; this example does not require a dynamic ThreadX
  byte pool.
- ThreadX uses SysTick at 100 ticks/second. Keep
  `TX_TIMER_TICKS_PER_SECOND` in `Core/Inc/tx_user.h`, `SYSTEM_CLOCK` and
  `SYSTICK_CYCLES` in `Core/Src/tx_initialize_low_level.S`, and application
  sleep intervals consistent.
- HAL's 1 ms tick is TIM6-based. Keep the HAL time-base implementation,
  `HAL_TIM_Base_MspInit`, and `TIM6_IRQHandler` together. Do not assign
  SysTick to HAL as well as ThreadX.
- ThreadX's Cortex-M33 GNU port supplies the scheduler's SVC/PendSV handlers;
  the low-level initialization assembly supplies ThreadX's SysTick handler.
  Do not add conflicting duplicate handlers in `stm32h5xx_it.c`.
- Add each peripheral's HAL driver source, MSP initialization, IRQ handler,
  and NVIC setup together when extending the example.

## Build, flash, and debug

```sh
make clean
make -j4
make flash
```

`make flash` uses the local `openocd/st_nucleo_h5.cfg`. In VS Code, install
Cortex-Debug and use the provided build/clean/flash tasks and
`Debug STM32H5 ThreadX (OpenOCD)` launch configuration. Update tool paths in
the Makefile and `.vscode/launch.json` if the toolchain/OpenOCD locations
differ.

Successful build, flash, and verify do not prove the first ThreadX context
switch completed. Check LED3, inspect `CFSR` for `INVPC` after a fault, and
confirm that `TX_SINGLE_MODE_SECURE` matches the processor's actual security
state.
