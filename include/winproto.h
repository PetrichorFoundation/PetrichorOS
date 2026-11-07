/* The window protocol between the desktop's terminal and Windows programs run by GrapeJuice.
 *
 * Every terminal gives its shell two extra pipes: file descriptor 3 carries messages from the program to the
 * desktop, descriptor 4 carries events back.  A program started from that terminal (and only those) inherits
 * them, so it can ask for windows.  Pixels are 0x00RRGGBB. */
#pragma once
#include "types.h"

#define PW_FD_UP   3                     /* program -> desktop */
#define PW_FD_DOWN 4                     /* desktop -> program */
#define PW_MAX_W   1000
#define PW_MAX_H   700
#define PW_MAX_ROW 900                   /* pixels per PRESENT message */

enum { PW_CREATE = 1,                    /* id, a = client width, b = client height; payload: title */
       PW_PRESENT,                       /* id, a = x, b = y, c = number of pixels; payload: a row segment */
       PW_TITLE,                         /* id; payload: new title */
       PW_DESTROY };                     /* id */

struct pw_msg {
    u32 type, id, a, b, c, d, len;       /* len: bytes of payload that follow */
};

enum { PE_MOUSE = 1,                     /* id, a = x, b = y, c = 0 move / 1 left down / 2 left up / 3 right down / 4 double click */
       PE_KEY,                           /* id, a = key code (abi.h), b = IEF_* flags */
       PE_CLOSE,                         /* id: the user asked to close the window */
       PE_WHEEL };                       /* id, a = +1 down / -1 up */

struct pw_event {
    u32 type, id, a, b, c, d;
};
