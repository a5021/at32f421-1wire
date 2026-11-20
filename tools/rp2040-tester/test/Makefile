CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -DLINEEDIT_NO_IO -I../src
TARGET := test_lineedit.exe

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): test_lineedit.c ../src/lineedit.c ../src/lineedit.h
	$(CC) $(CFLAGS) test_lineedit.c ../src/lineedit.c -o $@

clean:
	rm -f $(TARGET)
