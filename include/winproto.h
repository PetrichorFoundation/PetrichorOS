/* The window protocol between the desktop and separate programs (GrapeJuice's Windows programs, the browser
 * package, ...).
 *
 * File descriptor 3 carries messages from the program to the desktop, descriptor 4 carries events back.  Programs
 * started from a desktop terminal inherit them, and so do programs the desktop launches itself (dock, menus,
 * Files).  Pixels are 0x00RRGGBB.
 *
 * Two ways to show pixels:
 *   PW_CREATE + PW_PRESENT   the pixels travel through the pipe, row by row (small windows, GrapeJuice);
 *   PW_SHM_WINDOW + PW_DAMAGE the program draws into a shared-memory buffer (shm() in ulib.h) and only says which
 *                            part changed; the window can be resized (PE_RESIZE, answered with PW_BUFFER). */
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
       PW_DESTROY,                       /* id */
       PW_SHM_WINDOW,                    /* id, a = width, b = height, c = shm buffer id; payload: title */
       PW_DAMAGE,                        /* id, a = x, b = y, c = w, d = h: repaint that part of the buffer */
       PW_BUFFER,                        /* id, a = width, b = height, c = new shm buffer id (after PE_RESIZE) */
       PW_NOTIFY };                      /* payload: a message for the desktop's notification corner */

struct pw_msg {
    u32 type, id, a, b, c, d, len;       /* len: bytes of payload that follow */
};

enum { PE_MOUSE = 1,                     /* id, a = x, b = y, c = 0 move / 1 left down / 2 left up / 3 right down / 4 double click */
       PE_KEY,                           /* id, a = key code (abi.h), b = IEF_* flags */
       PE_CLOSE,                         /* id: the user asked to close the window */
       PE_WHEEL,                         /* id, a = +1 down / -1 up */
       PE_POINTER,                       /* shm windows: id, a = x, b = y, c = M_* kind (move, down, up, drag, double, right, middle) */
       PE_RESIZE };                      /* shm windows: id, a = new width, b = new height (send a PW_BUFFER) */

struct pw_event {
    u32 type, id, a, b, c, d;
};
