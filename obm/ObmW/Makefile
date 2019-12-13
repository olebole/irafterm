OBJS =  Arrow.o Board.o Button.o Common.o DrawIString.o DrawString.o \
        DrawingArea.o Frame.o Group.o Gterm.o HTML-PSformat.o HTML.o \
        HTMLformat.o HTMLimages.o HTMLjot.o HTMLlists.o HTMLparse.o \
        HTMLwidgets.o Icon.o Label.o Layout.o MenuBar.o MultiList.o \
        RadioGrp.o RowCol.o Scrollbar.o Slider2.o Tablist2Tabs.o TextWidth.o \
        Toggle.o cvtLong.o iconutil.o laygram.o laylex.o scroll.o strnchr.o \
        Tabs.o Gcs.o ListTree.o Separator.o Table.o Table3d.o TableUtil.o \
        Container.o color.o 

all: $(OBJS)

laygram.c: laygram.y
	$(YACC) -d -o $@ $^
	sed s/yy/LayYY/g -i laygram.[ch]

laylex.c: laylex.l
	$(LEX) -o $@ $^
	sed s/yy/LayYY/g -i laylex.c

laylex.o: laylex.c laygram.c

Gterm.o: Gterm.c GtermCnv.c GtermGraphics.c GtermImaging.c GtermCmap.c \
    GtermMapping.c GtermMarker.c GtermUtil.c GtermDebug.c

clean:
	rm -f $(OBJS) laygram.[ch] laylex.c
