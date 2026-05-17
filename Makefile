CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra
all: graphicsplitter

graphicsplitter: gio_processdata.o graphicsplitter.o gtermio_buffer.o gtermio_io.o v_write.o

clean:
	rm -f graphicsplitter gfx.log
