CC = gcc
CFLAGS := -lsqlite3 -std=c99 -pedantic -g $(CFLAGS)

all: ftag

ftag: ftag.c
	$(CC) ftag.c -o ftag $(CFLAGS)

clean:
	rm ftag

