#!/bin/bash

echo "=== Установка зависимостей ==="
sudo apt update
sudo apt install -y build-essential flex bison libssl-dev libelf-dev \
                    libncurses-dev bc debhelper git qemu-system-x86 \
                    linux-headers-$(uname -r)

echo "=== Скачивание и распаковка ядра 6.12.1 ==="
cd /tmp
    
echo "Скачивание linux-6.12.1.tar.xz..."
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.1.tar.xz
if [ $? -ne 0 ]; then
	echo "Ошибка при скачивании ядра"
	exit 1
fi
    
echo "Распаковка linux-6.12.1.tar.xz..."
tar -xf linux-6.12.1.tar.xz
if [ $? -ne 0 ]; then
	echo "Ошибка при распаковке"
	exit 1
fi
    
cd linux-6.12.1
    
echo "=== Настройка ядра ==="
make defconfig
    
echo "=== Сборка ядра ==="
make -j$(nproc) bzImage modules
    
if [ $? -eq 0 ]; then
	echo "Образ ядра: /tmp/linux-6.12.1/arch/x86/boot/bzImage"
	sudo make modules_install INSTALL_MOD_PATH=/tmp/simplefs_test
else
	echo "Ошибка при сборке ядра"
	exit 1
fi

echo "=== Готово ==="
