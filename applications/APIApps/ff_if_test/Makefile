ARCH ?= riscv32

LIBDASH_INC ?= ../../../libdash
LIBDASH_LIB ?= ../../../build/libdash

INCLUDES = -I $(LIBDASH_INC)
LIBS = -L $(LIBDASH_LIB)

APP_NAME = fft_test
SOURCES = fft_test.cpp

ifeq ($(ARCH), aarch64)
        CXX = aarch64-linux-gnu-g++
        CC = aarch64-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build-arm/libdash
else ifeq ($(ARCH), riscv64)
        CXX = riscv64-linux-gnu-g++
		CC = riscv64-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build_riscv/libdash
else ifeq ($(ARCH), riscv32)
        CXX = /media/demo/CEDR_LINUX/cva5-linux/LINUX_IMAGE/buildroot/output/host/bin/riscv32-buildroot-linux-gnu-g++
		CC = /media/demo/CEDR_LINUX/cva5-linux/LINUX_IMAGE/buildroot/output/host/bin/riscv32-buildroot-linux-gnu-gcc
        RUNTIME_LIBDIR = ../../../build/libdash
endif

$(APP_NAME)-$(ARCH).so: $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -fPIC $(SOURCES) -o $(APP_NAME)-$(ARCH).so

clean:
	-rm $(APP_NAME)-*.so
	-rm $(APP_NAME)-*.out
