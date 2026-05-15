CC := clang
CFLAGS := -Wall -Wextra -g
JSC := -framework JavaScriptCore

.PHONY: build run clean

build:
	$(CC) $(CFLAGS) -o kobun src/*.c $(JSC)

clean:
	rm -rf .build kobun
