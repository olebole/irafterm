# IRAF graphics wrapper

Traditionally, the interactive graphics of IRAF is done with the
"xgterm" program of the "x11iraf" package. xgterm is based on an old,
patched version of the "xterm" program from the X11 Consortium. The
x11iraf package is basically abandoned since 2008. The patches on the
xterm program are not documented.

The usage of an old, patched version of xterm has a number of
disadvantages. The main problem is that xterm is not really suited for
modern high resolution screens. Also, xterm lacks a number of modern
features that are nowadays expected on a terminal.

Therefore, this attempt tries to separate the graphics from the
terminal using a filtering wrapper. The wrapper catches all graphics
control sequences and sends them to the separate window. That way,
IRAF can still run within the default terminal.

The code is based on the "obm" subdirectory of the last x11iraf
version.
