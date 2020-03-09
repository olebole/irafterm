
CFLAGS += -I../obm
LDFLAGS += -L../obm -lobm -lXt -lX11 -lXpm -lXmu -lXext -lXaw -ltcl -lm

all: obmsh

obmsh: ../obm/libobm.a obmsh.o
	$(CC)  obmsh.o $(CFLAGS) $(LDFLAGS) -o $@

