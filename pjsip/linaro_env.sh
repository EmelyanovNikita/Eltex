#!/bin/bash
export LINARO_ROOT="/home/nikita/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu"
export PATH="$LINARO_ROOT/bin:$PATH"

# Компиляторы
export CC="/home/nikita/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc"
export CXX="/home/nikita/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-g++"

# Пути для заголовков
export C_INCLUDE_PATH="$LINARO_ROOT/aarch64-linux-gnu/libc/usr/include:$LINARO_ROOT/lib/gcc/aarch64-linux-gnu/7.5.0/include"

# Пути для библиотек
export LIBRARY_PATH="$LINARO_ROOT/aarch64-linux-gnu/libc/usr/lib"
export LDFLAGS="-L$LINARO_ROOT/aarch64-linux-gnu/libc/usr/lib -Wl,-rpath-link=$LINARO_ROOT/aarch64-linux-gnu/libc/lib"
