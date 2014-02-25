CC = gcc
CFLAGS = -lsqlite3 -std=c99 -g

all: ftag

ftag: ftag.o
	$(CC) ftag.c $(CFLAGS) -o ftag
	
ftag.o: ftag.c
	$(CC) -c ftag.c -o ftag.o $(CFLAGS)
	
clean:
	rm *.o ftag