# Context for Claude (running on omarchy)

Goal: run ST's VL53L9 example (STSW-IMG054, in `vendor/vl53l9-nucleo-n657-fmw-1.1.1`, unmodified;
`app/` + `lib/` must be imported from the ST zip, see vendor/README.md)
on NUCLEO-N657X0-Q + STEVAL-VL53L9, on Arch Linux (omarchy), with no ST tools / ST account.
User-facing steps are in README.md. The user writes Japanese.

## State as of 2026-11-16 (prepared on a Mac, not yet run on hardware)
- `Makefile` reproduces `platform/FSBL/.cproject` (defines, include paths, -O3, -mcmse, linker script).
  Verified in an `archlinux` container with arm-none-eabi-gcc 16.2.0: `ranging_i3c` and `ranging_csi`
  link cleanly (~145 KB at 0x34180400, AXISRAM2). `ranging_multi_i3c` intentionally `#error`s with one device.
- `scripts/load-ram.sh` (`make run`): OpenOCD `load_image` into SRAM, set PC to ELF entry, resume.
  **Never tested on hardware.** First thing to validate.
- `openocd/nucleo-n657x0.cfg`: stlink-dap + upstream `target/stm32n6x.cfg` (needs openocd-git; Arch's
  0.12.0 lacks it).

## Explorer (added 2026-11-17, not run on hardware)
- `apps/explorer/vl53l9_app.c` (our code, `make APP=explorer`; Makefile looks in `apps/<APP>` before the vendor app dir).
  I3C only. Text commands in via LPUART1 RX interrupt (HAL_UART_RxCpltCallback), out: log lines, `$J {json}` lines,
  binary packets `A5 5A 'V' '9' | u32 len | body | crc32`. Any `set` = stop sensor + destroy/recreate transform + restart.
- `web/index.html`: WebSerial UI, no deps; `#demo` fakes the firmware through the same parser. `make web` serves it.
- Untested on hardware: autonomous sync over I3C, baud switching (ST-LINK V3 VCP rates), transform recreate leaks.
- Hardware: STEVAL connects by FPC alone (CN6 ↔ J1, opposite-side-contact 22-pin 0.5 mm cable), J2 header unused.

## Hardware facts (from vendor README)
- Boot: JP1/BOOT0 1-2, JP2/BOOT1 2-3 = dev boot (RAM). 1-2/1-2 = boot from ext flash (needs signed image).
- Console: LPUART1 @ 921600 via ST-LINK VCP.
- Default device config: STEVAL on camera connector (I3C2 + CSI). X-NUCLEO via Arduino = `-DCONFIG_HW_X_NUCLEO_ARD`.
- Ext flash path (not set up): STM32_SigningTool_CLI + STM32_Programmer_CLI at 0x70000000 — needs ST download.

## If `make run` fails, things to try
- `reset halt` problems: try `reset_config srst_only srst_nogate connect_assert_srst`, or `-c "adapter speed 4000"`.
- Secure vs non-secure AP access: stm32n6x.cfg has `stm32n6x_ahb_ap_secure_access`. Image links at secure aliases 0x34xxxxxx.
- If the reset handler path misbehaves, use gdb (`arm-none-eabi-gdb`, `target extended-remote :3333`,
  `load`, `set $pc=Reset_Handler`, `continue`).
- ST-LINK firmware too old for N6 → update needs STLinkUpgrade (ST download); ask user.
- probe-rs (user prefers it): `make run-probe-rs` = `probe-rs run --chip STM32N657`. Target exists since
  0.32.0. Known bug probe-rs#4390: RAM-run images start with PRIMASK=1 + stale NVIC; fix PR #4703 open as of
  2026-11-14. `src/irq_unmask.c` (constructor, before main) works around it so vendor code stays untouched.
  Drop it once a fixed release is out. Firmware has no RTT/defmt; logs are UART only. Forum reports
  occasional ST-LINK SWD lockups on this board with probe-rs. User also likes Zephyr/defmt/Rust (optional).

## License
Repo is public. `vendor/.../app/` and `vendor/.../lib/` are SLA0111 and gitignored (placeholders only);
the user must import them with `scripts/import-st-package.sh <STSW-IMG054 zip>` before building.
Never commit those two directories.
