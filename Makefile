CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 $(shell pkg-config --cflags gtk4)
LDFLAGS = $(shell pkg-config --libs gtk4)

TARGET  = build/nomo
SRC     = src/main.c

.PHONY: all clean run deps install

all: $(TARGET)

$(TARGET): $(SRC) | build
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

build:
	mkdir -p build

clean:
	rm -rf build

run: all
	$(TARGET) &

deps:
	sudo apt-get update
	sudo apt-get install -y libgtk-4-dev build-essential

install: all
	cp -f $(TARGET) /home/igor/nomo/nomo
	@echo "Installed: /home/igor/nomo/nomo"
