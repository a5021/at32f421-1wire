# AT32F421F8P7 standalone 1-Wire driver (GCC, Make)

TARGET   := at32f421-1wire
BUILD    := build

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

MCU      := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft

INCLUDES := -Isrc -Ivendor/at32f421
DEFS     := -DAT32F421F8P7
DIAG_BUILD ?= 0
ifneq ($(DIAG_BUILD),0)
DEFS     += -DDIAG_BUILD
endif

CFLAGS   := $(MCU) -std=gnu11 -O2 -g3 -flto -Wall -Wextra -Wshadow \
            -ffunction-sections -fdata-sections $(INCLUDES) $(DEFS) -MMD -MP

LDSCRIPT := vendor/at32f421/AT32F421x8_FLASH.ld
LDFLAGS  := $(MCU) -flto -T$(LDSCRIPT) \
            --specs=nano.specs --specs=nosys.specs \
            -Wl,--gc-sections -Wl,--print-memory-usage \
            -Wl,-Map=$(BUILD)/$(TARGET).map

SRCS := \
	src/ow_crc.c \
	src/ow_hal.c \
	src/manager.c \
	src/onewire.c \
	src/ds18b20.c \
	src/ds2431.c \
	src/clock.c \
	app/i2c_hw.c \
	app/i2c_slave.c \
	app/main.c \
	vendor/at32f421/system_at32f421.c

ASM_SRC := vendor/at32f421/startup_at32f421.s

OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
OBJS += $(BUILD)/startup_at32f421.o

VPATH := src:app:vendor/at32f421

HOST_CC := gcc
PYTHON  ?= python
HOST_SRCS := \
	tests/run_tests.c \
	tests/test.c \
	tests/mock/ow_hal_mock.c \
	tests/mock/i2c_hw_mock.c \
	tests/test_i2c_slave.c \
	tests/test_crc.c \
	tests/test_timing.c \
	tests/test_encode.c \
	tests/test_bus.c \
	tests/test_search.c \
	tests/test_ds18b20.c \
	tests/test_ds2431.c \
	tests/test_state.c \
	tests/test_headers.c \
	app/i2c_slave.c \
	src/ow_crc.c \
	src/onewire.c \
	src/ds18b20.c \
	src/ds2431.c \
	src/manager.c \
	tests/test_manager.c
HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wshadow -Wno-unused-parameter \
               -Isrc -Iapp -Itests -Itests/mock -Itools/rp2040-tester/src

# Flashing / debugging through CMSIS-DAP (WCH-Link) with pyOCD + Artery DFP.
PYOCD        ?= pyocd
PACK         ?= tools/packs/ArteryTek.AT32F421_DFP.2.2.0.pack
PYOCD_TARGET ?= -at32f421f8p7
PROBE        ?= BC723C76F8E8
PYOCD_CONN   := --pack $(PACK) -t=$(PYOCD_TARGET) -u $(PROBE)

OPENOCD      ?= tools/openocd-at32/openocd.exe
OPENOCD_SCRIPTS_DIR ?= tools/openocd-at32/scripts

.PHONY: all clean test size flash flash-stlink erase gdbserver debug demo

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD):
	@mkdir $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/startup_at32f421.o: $(ASM_SRC) | $(BUILD)
	$(CC) $(MCU) -c -x assembler-with-cpp $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

test: $(BUILD)/test_all
	$(BUILD)/test_all
	$(PYTHON) tools/check_regmap.py

$(BUILD)/test_all: $(HOST_SRCS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_SRCS) -o $@

flash: $(BUILD)/$(TARGET).elf
	$(PYOCD) flash $(PYOCD_CONN) -f 1M -e auto $<

flash-stlink: $(BUILD)/$(TARGET).elf
	OPENOCD_SCRIPTS=$(OPENOCD_SCRIPTS_DIR) $(OPENOCD) \
		-f interface/stlink.cfg -c "transport select hla_swd" \
		-f target/at32f421xx.cfg -c "adapter speed 1000" \
		-c "program $< verify reset exit"

demo:
	python tools/onewire_demo.py

erase:
	$(PYOCD) erase $(PYOCD_CONN) -e chip

gdbserver: $(BUILD)/$(TARGET).elf
	$(PYOCD) gdbserver $(PYOCD_CONN) -f 1M

debug: $(BUILD)/$(TARGET).elf
	arm-none-eabi-gdb $< -ex "target extended-remote :3333" -ex "monitor reset halt"

clean:
	@rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
