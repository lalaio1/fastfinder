# FastFinder - build targets
#
#   make          -> Linux binary (fastfinder), portable: SSE2 baseline with
#                    automatic AVX2 at runtime on capable CPUs
#   make NATIVE=1 -> Linux binary tuned for this exact CPU (-march=native);
#                    fastest, but the binary only runs on CPUs with the same
#                    instruction sets
#   make windows  -> Windows binary (fastfinder.exe), needs mingw-w64
#   make clean
#
# Override anything you like, for example:
#   make OPT="-O3 -flto -DNDEBUG"
#   make WIN_CC=x86_64-w64-mingw32-gcc-posix windows

CC         ?= gcc
WIN_CC     ?= x86_64-w64-mingw32-gcc

ifeq ($(NATIVE),1)
NATIVE_FLAG = -march=native
endif

OPT        ?= -O3 -flto -DNDEBUG
CFLAGS     ?= -std=c11 -Wall -Wextra $(OPT) $(NATIVE_FLAG)
WIN_CFLAGS ?= -std=c11 -Wall -Wextra $(OPT)

.PHONY: all linux windows clean

all: linux

linux: fastfinder
fastfinder: fastfinder.c
	$(CC) $(CFLAGS) -pthread -s -o $@ $<

windows: fastfinder.exe
fastfinder.exe: fastfinder.c
	$(WIN_CC) $(WIN_CFLAGS) -static -s -o $@ $<

clean:
	rm -f fastfinder fastfinder.exe
