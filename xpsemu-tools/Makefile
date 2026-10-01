# XPSemu Tools - standalone app-jailbreak daemon.
# Build on Linux (WSL2 Ubuntu) with the ps5-payload-sdk:
#   export PS5_PAYLOAD_SDK=~/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk
#   make
# Output: xpsemu_tools.elf - load it together with kstuff (e.g. via autoload).

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := xpsemu_tools.elf

CFLAGS := -O2 -Wall -Wextra -std=c11
LDLIBS := -lkernel_sys -lSceNotification

all: $(ELF)

$(ELF): src/main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(ELF)

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean deploy
