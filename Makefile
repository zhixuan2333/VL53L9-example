# Builds the ST vl53l9-nucleo-n657 firmware (STSW-IMG054) without STM32CubeIDE.
# Flags/sources mirror vendor/.../platform/FSBL/.cproject.
#
#   make                      -> build/ranging_i3c/vl53l9.elf
#   make APP=ranging_csi      -> other apps: ranging_csi, ranging_multi_i3c
#   make APP=explorer         -> apps/explorer: all settings live from the web UI (make web)
#   make run                  -> load into SRAM and run via OpenOCD (JP2/BOOT1 = 2-3)
#   make run-probe-rs         -> same via probe-rs
#   make monitor              -> serial console, 921600 baud
#   make EXTRA_DEFS=-DCONF_PRINT_FRAME=1   -> ASCII depth frames on the console
#   make web                  -> serve web/ on http://localhost:8000 (WebSerial UI for APP=explorer)

PKG   ?= vendor/vl53l9-nucleo-n657-fmw-1.1.1
APP   ?= ranging_i3c
BUILD := build/$(APP)
TARGET := $(BUILD)/vl53l9
APP_DIR := $(firstword $(wildcard apps/$(APP)) $(PKG)/app/$(APP))
TTY   ?= $(firstword $(wildcard /dev/serial/by-id/*STLINK* /dev/ttyACM0))

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

MCU := -mcpu=cortex-m55 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -mcmse

DEFS := -DUSE_HAL_DRIVER -DSTM32N657xx -DUSE_NUCLEO_64 -DVL53L9_TRANSFORM_LIGHT=1 $(EXTRA_DEFS)

TRANSFORM := $(PKG)/lib/transform/vl53l9-transform-c
INCS := -I$(PKG)/platform/FSBL/Core/Inc \
        -I$(PKG)/platform/Drivers/STM32N6xx_HAL_Driver/Inc \
        -I$(PKG)/platform/Drivers/STM32N6xx_HAL_Driver/Inc/Legacy \
        -I$(PKG)/platform/Drivers/CMSIS/Device/ST/STM32N6xx/Include \
        -I$(PKG)/platform/Drivers/BSP/STM32N6xx_Nucleo \
        -I$(PKG)/platform/Drivers/CMSIS/Include \
        -I$(TRANSFORM)/vl53l9-transform-c-lib/include/public \
        -I$(TRANSFORM)/vl53l9-transform-c-lib/include/public/vl53l9-transform-c \
        -I$(TRANSFORM)/vl53l9-transform-c-lib/include/private \
        -I$(TRANSFORM)/vl53l9-transform-c-algos/include \
        -I$(PKG)/lib/media-object/media-c/header-lib/include/public \
        -I$(PKG)/lib/media-object/transform-c/header-lib/include/public \
        -I$(PKG)/interface/vl53l9 \
        -I$(PKG)/drivers/vl53l9 \
        -I$(PKG)/app \
        -I$(PKG)/app/vl53l9_utils \
        -I$(PKG)/app/vl53l9_app_common

CFLAGS  := $(MCU) $(DEFS) $(INCS) -O3 -g3 -Wall -ffunction-sections -fdata-sections -std=gnu11 -MMD -MP
LDFLAGS := $(MCU) -T$(PKG)/platform/FSBL/STM32N657X0HXQ.ld \
           --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
           -Wl,-Map=$(TARGET).map -Wl,--no-warn-rwx-segments \
           -Wl,--cmse-implib -Wl,--out-implib=$(BUILD)/secure_nsclib.o \
           -Wl,--start-group -lc -lm -Wl,--end-group

SRCS := $(wildcard src/*.c) \
        $(wildcard $(PKG)/platform/FSBL/Core/Src/*.c) \
        $(wildcard $(PKG)/platform/Drivers/STM32N6xx_HAL_Driver/Src/*.c) \
        $(PKG)/platform/Drivers/BSP/STM32N6xx_Nucleo/stm32n6xx_nucleo.c \
        $(APP_DIR)/vl53l9_app.c \
        $(PKG)/app/vl53l9_app_common/vl53l9_app_common.c \
        $(PKG)/app/vl53l9_utils/vl53l9_utils.c \
        $(PKG)/drivers/vl53l9/vl53l9.c \
        $(wildcard $(PKG)/interface/platform/*.c) \
        $(wildcard $(PKG)/interface/vl53l9/*.c) \
        $(wildcard $(TRANSFORM)/vl53l9-transform-c-algos/src/*.c) \
        $(wildcard $(TRANSFORM)/vl53l9-transform-c-lib/src/*.c)
ASMS := $(PKG)/platform/FSBL/Core/Startup/startup_stm32n657x0hxq_fsbl.s

ifeq ($(wildcard $(PKG)/lib/transform),)
$(error $(PKG)/app and lib are missing; see vendor/README.md (scripts/import-st-package.sh <zip>))
endif
ifeq ($(wildcard $(APP_DIR)/vl53l9_app.c),)
$(error unknown APP=$(APP); pick one of: $(notdir $(wildcard $(PKG)/app/ranging_*) $(wildcard apps/*)))
endif

OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(ASMS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASMS)))

all: $(TARGET).elf $(TARGET).bin

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(MCU) -x assembler-with-cpp -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

run: $(TARGET).elf
	scripts/load-ram.sh $<

run-probe-rs: $(TARGET).elf
	probe-rs run --chip STM32N657 $<

monitor:
	picocom -b 921600 $(TTY)

web:
	python3 -m http.server --bind 127.0.0.1 --directory web 8000

clean:
	rm -rf build

-include $(OBJS:.o=.d)

.PHONY: all run run-probe-rs monitor web clean
