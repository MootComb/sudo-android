CC ?= gcc

CFLAGS = -static -O3 -flto=auto \
         -fomit-frame-pointer \
         -fno-unwind-tables -fno-asynchronous-unwind-tables \
         -ffunction-sections -fdata-sections \
         -fPIE -pie \
         -fstack-protector-strong -fstack-clash-protection \
         -D_FORTIFY_SOURCE=3 \
         -fvisibility=hidden -fno-ident \
         -ftree-vectorize -funroll-loops

LDFLAGS = -Wl,--gc-sections -Wl,--strip-all \
          -Wl,-z,relro,-z,now,-z,noexecstack \
          -Wl,-z,defs \
          -Wl,--build-id=none \
          -s

SRC_DIR = src
TARGET = sudo
SOURCE = $(SRC_DIR)/sudo.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCE)
	@echo "Build completed with $(CC)!"

clean:
	rm -f $(TARGET)

.PHONY: all clean
