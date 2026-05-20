#!/bin/bash
set -euo pipefail
MODULE_NAME="simplefs"
MODULE_FILE="./kernel/simplefs.ko"
CTL="./user/simplefsctl"
IMG="./simplefs.img"
MNT="/mnt/simplefs_test"
IMG_SIZE_MB=16
cleanup() {
  set +e
  echo "=== Очистка ==="
  mountpoint -q "$MNT" && sudo umount "$MNT"
  [ -n "${LOOPDEV:-}" ] && sudo losetup -d "$LOOPDEV"
  lsmod | grep -q "^${MODULE_NAME}" && sudo rmmod "$MODULE_NAME"
}
trap cleanup EXIT
echo "=== Сборка проекта ==="
make clean
make CC=gcc-12
[ -f "$MODULE_FILE" ] || { echo "Ошибка: $MODULE_FILE не найден"; exit 1; }
[ -x "$CTL" ] || { echo "Ошибка: $CTL не найден или не исполняемый"; exit 1; }
echo "=== Выгрузка старого модуля, если он загружен ==="
if lsmod | grep -q "^${MODULE_NAME}"; then
	echo "=== Выгрузка старого модуля ==="
	sudo rmmod "$MODULE_NAME"
fi
echo "=== Создание тестового образа ==="
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_SIZE_MB" status=progress
echo "=== Подключение loop-устройства ==="
LOOPDEV=$(sudo losetup --find --show "$IMG")
echo "Loop device: $LOOPDEV"
echo "=== Загрузка модуля ==="
sudo insmod "$MODULE_FILE" \
  disk_name="$LOOPDEV" \
  sb_primary_sector=0 \
  sb_backup_sector=8 \
  max_name_len=32 \
  max_file_sectors=4
echo "=== Создание точки монтирования ==="
sudo mkdir -p "$MNT"
echo "=== Монтирование simplefs ==="
sudo mount -t simplefs "$LOOPDEV" "$MNT"
echo "=== Проверка файлов ==="
ls -la "$MNT" | {
	head -5
	echo "..."
	tail -5
} || true
echo "=== Тест demo ==="
sudo "$CTL" demo "$MNT"
echo "=== Тест hashes ==="
sudo "$CTL" hashes "$MNT" | {
	head -5
	echo "..."
	tail -5
} || true
echo "=== Тест mapping (f0) ==="
sudo "$CTL" mapping "$MNT" f0
echo "=== Тест zero ==="
sudo "$CTL" zero "$MNT"
echo "=== Тест erase ==="
sudo "$CTL" erase "$MNT"
echo "=== dmesg simplefs ==="
sudo dmesg | tail -200 | grep -Ei "simplefs|error|bug|oops|panic" || true
echo "=== Готово ==="
