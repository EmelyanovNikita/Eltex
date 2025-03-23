# Получаем список всех .c файлов в текущей директории
SOURCES = $(wildcard *.c)

# Генерируем список выходных файлов (заменяем .c на пустую строку)
OUTPUTS = $(SOURCES:.c=)

# Флаги компиляции и библиотеки
CFLAGS = $(shell pkg-config --cflags libpjproject)
LIBS = $(shell pkg-config --libs libpjproject)

# Цель по умолчанию: собираем все выходные файлы
all: $(OUTPUTS)

# Правило для сборки каждого файла
%: %.c
	gcc -o $@ $< $(CFLAGS) $(LIBS)

# Очистка
clean:
	rm -f $(OUTPUTS)
