.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -o code main.c buddy.c -O2 -Wno-int-conversion

clean:
	rm -f code test