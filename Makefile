# Прямой путь к компилятору (исправь, если папка называется иначе)
CC = C:/gcc-arm/bin/aarch64-none-elf-gcc
OBJCOPY = C:/gcc-arm/bin/aarch64-none-elf-objcopy

CFLAGS = -Wall -O2 -ffreestanding -nostdlib -nostartfiles
ASFLAGS = 

all: H_Exo_Aleph.img

# 1. Компилируем ассемблер и Си
boot.o: boot.s
	$(CC) $(ASFLAGS) -c boot.s -o boot.o

mmu.o: mmu.s
	$(CC) $(ASFLAGS) -c mmu.s -o mmu.o

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

# 2. Линкуем в ELF
kernel.elf: boot.o mmu.o main.o
	$(CC) -T linker.ld -o kernel.elf boot.o mmu.o main.o $(CFLAGS)

# 3. Извлекаем чистый бинарник и подписываем его (stage2 image, требует внешнего idbloader на секторе 64)
H_Exo_Aleph.img: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin
	py prepare_image.py kernel.bin H_Exo_Aleph.img

clean:
	rm -f *.o *.elf *.bin *.img