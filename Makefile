CC = gcc
CFLAGS := -lsqlite3 -std=c99 -pedantic -g $(CFLAGS)

all: ftag

ftag: ftag.c CuTest.c ftag.h
	$(CC) ftag.c CuTest.c -o ftag $(CFLAGS)

clean:
	rm ftag

