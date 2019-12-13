/*
 * GTERMIO.H -- Public definitions for the gterm i/o protocol module.
 */

#include <X11/Intrinsic.h>

struct GT_function {
	char *name;		/* callback name */
	int (*func)();		/* callback function */
	XtPointer data;		/* callback data */
};
