ARCH ?= riscv32

LIBDASH_INC ?= ../../../libdash
LIBDASH_LIB ?= ../../../build_riscv32/libdash

INCLUDES = -I $(LIBDASH_INC)
LIBS = -L $(LIBDASH_LIB)

APP_NAME = radar_correlator
SOURCES = radar_correlator.c
SOURCES_ZIP = radar_correlator_zip.c

ifeq ($(ARCH), aarch64)
        CXX = aarch64-linux-gnu-g++
        CC = aarch64-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build-arm/libdash
else ifeq ($(ARCH), riscv64)
        CXX = riscv64-linux-gnu-g++
		CC = riscv64-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build_riscv/libdash
else ifeq ($(ARCH), riscv32)
        CXX = /media/demo/fix/cva5-linux/LINUX_IMAGE/buildroot/output/host/bin/riscv32-buildroot-linux-gnu-g++
		CC = /media/demo/fix/cva5-linux/LINUX_IMAGE/buildroot/output/host/bin/riscv32-buildroot-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build_riscv32/libdash
endif

$(APP_NAME)-$(ARCH).so: $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -fPIC $(SOURCES) -o $(APP_NAME)-$(ARCH).so

zip: $(SOURCES_ZIP)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -fPIC $(SOURCES_ZIP) -o $(APP_NAME)-zip-$(ARCH).so

.PHONY: standalone
standalone: $(APP_NAME)-$(ARCH).out
$(APP_NAME)-$(ARCH).out: $(SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) $(LIBS) $(SOURCES) -l:libdash.a -lm -o $(APP_NAME)-$(ARCH).out

clean:
	-rm $(APP_NAME)-*.so
	-rm $(APP_NAME)-*.out
