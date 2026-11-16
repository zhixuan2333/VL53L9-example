# vendor/

`vl53l9-nucleo-n657-fmw-1.1.1/` is ST's **STSW-IMG054** firmware package for NUCLEO-N657X0-Q, unmodified.

| Part | License | In this repo |
|---|---|---|
| `platform/` (CMSIS, STM32N6 HAL, BSP, CubeMX FSBL) | Apache-2.0 / BSD-3-Clause | yes |
| `drivers/vl53l9/`, `interface/` | BSD-3-Clause | yes |
| `app/` (example apps) | SLA0111 | **no**, placeholder only |
| `lib/` (vl53l9-transform-c, media-c, transform-c) | SLA0111 | **no**, placeholder only |

To fill in `app/` and `lib/`:

1. Download STSW-IMG054 from https://www.st.com/en/embedded-software/stsw-img054.html
2. `scripts/import-st-package.sh <path/to/zip>`
