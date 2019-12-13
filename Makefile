OBJS = ptywrapper.o gtermio.o

all: irafterm

libobm.a:
	$(MAKE) -C obm libobm.a

irafterm: $(OBJS) libobm.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) -Lobm -lobm -lXt -lX11 -lXpm -lXmu -lXext -lXaw -ltcl -lm

clean:
	rm -f irafterm $(OBJS)
	$(MAKE) -C obm clean
