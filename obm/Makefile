OBJS = Obm.o client.o param.o server.o widget.o gterm.o marker.o html.o geom.o
ALL_OBJS = ObmW/[A-Za-z]*.o $(OBJS)

all: libobm.a

libobm.a: $(OBJS)
	$(MAKE) -C ObmW
	ar cr $@ $(ALL_OBJS)

clean:
	rm -f $(OBJS) libobm.a
	$(MAKE) -C ObmW clean
