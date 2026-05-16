CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra
all: graphicsplitter

graphicsplitter: graphicsplitter.c
	$(CC) $(CFLAGS) -o $@ graphicsplitter.c

clean:
	rm -f graphicsplitter gfx.log
