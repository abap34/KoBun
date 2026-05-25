CC := clang
CFLAGS := -Wall -Wextra -g
JSC := -framework JavaScriptCore

.PHONY: build run clean format

build:
	$(CC) $(CFLAGS) -o kobun main.c $(JSC)

format:
	xcrun clang-format -i main.c

clean:
	rm -rf .build kobun
