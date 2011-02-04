/*
 * Copyright (c) 2006 Josef Cejka
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup console
 * @{
 */
/** @file
 */

#include <libc.h>
#include <ipc/kbd.h>
#include <io/keycode.h>
#include <ipc/mouse.h>
#include <ipc/fb.h>
#include <ipc/services.h>
#include <ipc/ns.h>
#include <errno.h>
#include <ipc/console.h>
#include <unistd.h>
#include <async.h>
#include <adt/fifo.h>
#include <sys/mman.h>
#include <stdio.h>
#include <str.h>
#include <sysinfo.h>
#include <event.h>
#include <devmap.h>
#include <fcntl.h>
#include <vfs/vfs.h>
#include <fibril_synch.h>
#include <io/style.h>
#include <io/screenbuffer.h>

#include "console.h"
#include "gcons.h"
#include "keybuffer.h"


#define NAME       "console"
#define NAMESPACE  "term"

/** Phone to the keyboard driver. */
static int kbd_phone;

/** Phone to the mouse driver. */
static int mouse_phone;

/** Information about framebuffer */
struct {
	int phone;           /**< Framebuffer phone */
	sysarg_t cols;       /**< Framebuffer columns */
	sysarg_t rows;       /**< Framebuffer rows */
	sysarg_t color_cap;  /**< Color capabilities (FB_CCAP_xxx) */
} fb_info;

typedef struct {
	size_t index;             /**< Console index */
	size_t refcount;          /**< Connection reference count */
	devmap_handle_t devmap_handle;  /**< Device handle */
	keybuffer_t keybuffer;    /**< Buffer for incoming keys. */
	screenbuffer_t scr;       /**< Screenbuffer for saving screen
	                               contents and related settings. */
} console_t;

/** Array of data for virtual consoles */
static console_t consoles[CONSOLE_COUNT];

static console_t *active_console = &consoles[0];
static console_t *prev_console = &consoles[0];
static console_t *kernel_console = &consoles[KERNEL_CONSOLE];

/** Pointer to memory shared with framebufer used for
    faster virtual console switching */
static keyfield_t *interbuffer = NULL;

/** Information on row-span yet unsent to FB driver. */
struct {
	sysarg_t col;  /**< Leftmost column of the span. */
	sysarg_t row;  /**< Row where the span lies. */
	sysarg_t cnt;  /**< Width of the span. */
} fb_pending;

static FIBRIL_MUTEX_INITIALIZE(input_mutex);
static FIBRIL_CONDVAR_INITIALIZE(input_cv);

static void curs_visibility(bool visible)
{
	async_msg_1(fb_info.phone, FB_CURSOR_VISIBILITY, visible); 
}

static void curs_hide_sync(void)
{
	async_req_1_0(fb_info.phone, FB_CURSOR_VISIBILITY, false); 
}

static void curs_goto(sysarg_t x, sysarg_t y)
{
	async_msg_2(fb_info.phone, FB_CURSOR_GOTO, x, y);
}

static void screen_clear(void)
{
	async_msg_0(fb_info.phone, FB_CLEAR);
}

static void screen_yield(void)
{
	async_req_0_0(fb_info.phone, FB_SCREEN_YIELD);
}

static void screen_reclaim(void)
{
	async_req_0_0(fb_info.phone, FB_SCREEN_RECLAIM);
}

static void kbd_yield(void)
{
	async_req_0_0(kbd_phone, KBD_YIELD);
}

static void kbd_reclaim(void)
{
	async_req_0_0(kbd_phone, KBD_RECLAIM);
}

static void set_style(uint8_t style)
{
	async_msg_1(fb_info.phone, FB_SET_STYLE, style);
}

static void set_color(uint8_t fgcolor, uint8_t bgcolor, uint8_t flags)
{
	async_msg_3(fb_info.phone, FB_SET_COLOR, fgcolor, bgcolor, flags);
}

static void set_rgb_color(uint32_t fgcolor, uint32_t bgcolor)
{
	async_msg_2(fb_info.phone, FB_SET_RGB_COLOR, fgcolor, bgcolor); 
}

static void set_attrs(attrs_t *attrs)
{
	switch (attrs->t) {
	case at_style:
		set_style(attrs->a.s.style);
		break;
	case at_idx:
		set_color(attrs->a.i.fg_color, attrs->a.i.bg_color,
		    attrs->a.i.flags);
		break;
	case at_rgb:
		set_rgb_color(attrs->a.r.fg_color, attrs->a.r.bg_color);
		break;
	}
}

static int ccap_fb_to_con(sysarg_t ccap_fb, sysarg_t *ccap_con)
{
	switch (ccap_fb) {
	case FB_CCAP_NONE:
		*ccap_con = CONSOLE_CCAP_NONE;
		break;
	case FB_CCAP_STYLE:
		*ccap_con = CONSOLE_CCAP_STYLE;
		break;
	case FB_CCAP_INDEXED:
		*ccap_con = CONSOLE_CCAP_INDEXED;
		break;
	case FB_CCAP_RGB:
		*ccap_con = CONSOLE_CCAP_RGB;
		break;
	default:
		return EINVAL;
	}
	
	return EOK;
}

/** Send an area of screenbuffer to the FB driver. */
static void fb_update_area(console_t *cons, sysarg_t x0, sysarg_t y0, sysarg_t width, sysarg_t height)
{
	if (interbuffer) {
		sysarg_t x;
		sysarg_t y;
		
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				interbuffer[y * width + x] =
				    *get_field_at(&cons->scr, x0 + x, y0 + y);
			}
		}
		
		async_req_4_0(fb_info.phone, FB_DRAW_TEXT_DATA,
		    x0, y0, width, height);
	}
}

/** Flush pending cells to FB. */
static void fb_pending_flush(void)
{
	if (fb_pending.cnt > 0) {
		fb_update_area(active_console, fb_pending.col,
		    fb_pending.row, fb_pending.cnt, 1);
		fb_pending.cnt = 0;
	}
}

/** Mark a character cell as changed.
 *
 * This adds the cell to the pending rowspan if possible. Otherwise
 * the old span is flushed first.
 *
 */
static void cell_mark_changed(sysarg_t col, sysarg_t row)
{
	if (fb_pending.cnt != 0) {
		if ((col != fb_pending.col + fb_pending.cnt)
		    || (row != fb_pending.row)) {
			fb_pending_flush();
		}
	}
	
	if (fb_pending.cnt == 0) {
		fb_pending.col = col;
		fb_pending.row = row;
	}
	
	fb_pending.cnt++;
}

/** Print a character to the active VC with buffering. */
static void fb_putchar(wchar_t c, sysarg_t col, sysarg_t row)
{
	async_msg_3(fb_info.phone, FB_PUTCHAR, c, col, row);
}

/** Process a character from the client (TTY emulation). */
static void write_char(console_t *cons, wchar_t ch)
{
	bool flush_cursor = false;
	
	switch (ch) {
	case '\n':
		fb_pending_flush();
		flush_cursor = true;
		cons->scr.position_y++;
		cons->scr.position_x = 0;
		break;
	case '\r':
		break;
	case '\t':
		cons->scr.position_x += 8;
		cons->scr.position_x -= cons->scr.position_x % 8;
		break;
	case '\b':
		if (cons->scr.position_x == 0)
			break;
		cons->scr.position_x--;
		if (cons == active_console)
			cell_mark_changed(cons->scr.position_x, cons->scr.position_y);
		screenbuffer_putchar(&cons->scr, ' ');
		break;
	default:
		if (cons == active_console)
			cell_mark_changed(cons->scr.position_x, cons->scr.position_y);
		
		screenbuffer_putchar(&cons->scr, ch);
		cons->scr.position_x++;
	}
	
	if (cons->scr.position_x >= cons->scr.size_x) {
		flush_cursor = true;
		cons->scr.position_y++;
	}
	
	if (cons->scr.position_y >= cons->scr.size_y) {
		fb_pending_flush();
		cons->scr.position_y = cons->scr.size_y - 1;
		screenbuffer_clear_line(&cons->scr, cons->scr.top_line);
		cons->scr.top_line = (cons->scr.top_line + 1) % cons->scr.size_y;
		
		if (cons == active_console)
			async_msg_1(fb_info.phone, FB_SCROLL, 1);
	}
	
	if (cons == active_console && flush_cursor)
		curs_goto(cons->scr.position_x, cons->scr.position_y);
	cons->scr.position_x = cons->scr.position_x % cons->scr.size_x;
}

/** Switch to new console */
static void change_console(console_t *cons)
{
	if (cons == active_console) {
		return;
	}
	
	fb_pending_flush();
	
	if (cons == kernel_console) {
		async_serialize_start();
		curs_hide_sync();
		gcons_in_kernel();
		screen_yield();
		kbd_yield();
		async_serialize_end();
		
		if (__SYSCALL0(SYS_DEBUG_ENABLE_CONSOLE)) {
			prev_console = active_console;
			active_console = kernel_console;
		} else
			cons = active_console;
	}
	
	if (cons != kernel_console) {
		async_serialize_start();
		
		if (active_console == kernel_console) {
			screen_reclaim();
			kbd_reclaim();
			gcons_redraw_console();
		}
		
		active_console = cons;
		gcons_change_console(cons->index);
		
		set_attrs(&cons->scr.attrs);
		curs_visibility(false);
		
		sysarg_t x;
		sysarg_t y;
		int rc = 0;
		
		if (interbuffer) {
			for (y = 0; y < cons->scr.size_y; y++) {
				for (x = 0; x < cons->scr.size_x; x++) {
					interbuffer[y * cons->scr.size_x + x] =
					    *get_field_at(&cons->scr, x, y);
				}
			}
			
			/* This call can preempt, but we are already at the end */
			rc = async_req_4_0(fb_info.phone, FB_DRAW_TEXT_DATA,
			    0, 0, cons->scr.size_x,
			    cons->scr.size_y);
		}
		
		if ((!interbuffer) || (rc != 0)) {
			set_attrs(&cons->scr.attrs);
			screen_clear();
			
			for (y = 0; y < cons->scr.size_y; y++)
				for (x = 0; x < cons->scr.size_x; x++) {
					keyfield_t *field = get_field_at(&cons->scr, x, y);
					
					if (!attrs_same(cons->scr.attrs, field->attrs))
						set_attrs(&field->attrs);
					
					cons->scr.attrs = field->attrs;
					if ((field->character == ' ') &&
					    (attrs_same(field->attrs, cons->scr.attrs)))
						continue;
					
					fb_putchar(field->character, x, y);
				}
		}
		
		curs_goto(cons->scr.position_x, cons->scr.position_y);
		curs_visibility(cons->scr.is_cursor_visible);
		
		async_serialize_end();
	}
}

/** Handler for keyboard */
static void keyboard_events(ipc_callid_t iid, ipc_call_t *icall)
{
	/* Ignore parameters, the connection is already opened */
	while (true) {
		ipc_call_t call;
		ipc_callid_t callid = async_get_call(&call);
		
		int retval;
		console_event_t ev;
		
		switch (IPC_GET_IMETHOD(call)) {
		case IPC_M_PHONE_HUNGUP:
			/* TODO: Handle hangup */
			return;
		case KBD_EVENT:
			/* Got event from keyboard driver. */
			retval = 0;
			ev.type = IPC_GET_ARG1(call);
			ev.key = IPC_GET_ARG2(call);
			ev.mods = IPC_GET_ARG3(call);
			ev.c = IPC_GET_ARG4(call);
			
			if ((ev.key >= KC_F1) && (ev.key < KC_F1 +
			    CONSOLE_COUNT) && ((ev.mods & KM_CTRL) == 0)) {
				if (ev.key == KC_F1 + KERNEL_CONSOLE)
					change_console(kernel_console);
				else
					change_console(&consoles[ev.key - KC_F1]);
				break;
			}
			
			fibril_mutex_lock(&input_mutex);
			keybuffer_push(&active_console->keybuffer, &ev);
			fibril_condvar_broadcast(&input_cv);
			fibril_mutex_unlock(&input_mutex);
			break;
		default:
			retval = ENOENT;
		}
		async_answer_0(callid, retval);
	}
}

/** Handler for mouse events */
static void mouse_events(ipc_callid_t iid, ipc_call_t *icall)
{
	/* Ignore parameters, the connection is already opened */
	while (true) {
		ipc_call_t call;
		ipc_callid_t callid = async_get_call(&call);
		
		int retval;
		
		switch (IPC_GET_IMETHOD(call)) {
		case IPC_M_PHONE_HUNGUP:
			/* TODO: Handle hangup */
			return;
		case MEVENT_BUTTON:
			if (IPC_GET_ARG1(call) == 1) {
				int newcon = gcons_mouse_btn((bool) IPC_GET_ARG2(call));
				if (newcon != -1) {
					change_console(&consoles[newcon]);
				}
			}
			retval = 0;
			break;
		case MEVENT_MOVE:
			gcons_mouse_move((int) IPC_GET_ARG1(call),
			    (int) IPC_GET_ARG2(call));
			retval = 0;
			break;
		default:
			retval = ENOENT;
		}

		async_answer_0(callid, retval);
	}
}

static void cons_write(console_t *cons, ipc_callid_t rid, ipc_call_t *request)
{
	void *buf;
	size_t size;
	int rc = async_data_write_accept(&buf, false, 0, 0, 0, &size);
	
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
	
	async_serialize_start();
	
	size_t off = 0;
	while (off < size) {
		wchar_t ch = str_decode(buf, &off, size);
		write_char(cons, ch);
	}
	
	async_serialize_end();
	
	gcons_notify_char(cons->index);
	async_answer_1(rid, EOK, size);
	
	free(buf);
}

static void cons_read(console_t *cons, ipc_callid_t rid, ipc_call_t *request)
{
	ipc_callid_t callid;
	size_t size;
	if (!async_data_read_receive(&callid, &size)) {
		async_answer_0(callid, EINVAL);
		async_answer_0(rid, EINVAL);
		return;
	}
	
	char *buf = (char *) malloc(size);
	if (buf == NULL) {
		async_answer_0(callid, ENOMEM);
		async_answer_0(rid, ENOMEM);
		return;
	}
	
	size_t pos = 0;
	console_event_t ev;
	fibril_mutex_lock(&input_mutex);
	
recheck:
	while ((keybuffer_pop(&cons->keybuffer, &ev)) && (pos < size)) {
		if (ev.type == KEY_PRESS) {
			buf[pos] = ev.c;
			pos++;
		}
	}
	
	if (pos == size) {
		(void) async_data_read_finalize(callid, buf, size);
		async_answer_1(rid, EOK, size);
		free(buf);
	} else {
		fibril_condvar_wait(&input_cv, &input_mutex);
		goto recheck;
	}
	
	fibril_mutex_unlock(&input_mutex);
}

static void cons_get_event(console_t *cons, ipc_callid_t rid, ipc_call_t *request)
{
	console_event_t ev;
	
	fibril_mutex_lock(&input_mutex);
	
recheck:
	if (keybuffer_pop(&cons->keybuffer, &ev)) {
		async_answer_4(rid, EOK, ev.type, ev.key, ev.mods, ev.c);
	} else {
		fibril_condvar_wait(&input_cv, &input_mutex);
		goto recheck;
	}
	
	fibril_mutex_unlock(&input_mutex);
}

/** Default thread for new connections */
static void client_connection(ipc_callid_t iid, ipc_call_t *icall)
{
	console_t *cons = NULL;
	
	size_t i;
	for (i = 0; i < CONSOLE_COUNT; i++) {
		if (i == KERNEL_CONSOLE)
			continue;
		
		if (consoles[i].devmap_handle == (devmap_handle_t) IPC_GET_ARG1(*icall)) {
			cons = &consoles[i];
			break;
		}
	}
	
	if (cons == NULL) {
		async_answer_0(iid, ENOENT);
		return;
	}
	
	ipc_callid_t callid;
	ipc_call_t call;
	sysarg_t arg1;
	sysarg_t arg2;
	sysarg_t arg3;
	
	int rc;
	
	async_serialize_start();
	if (cons->refcount == 0)
		gcons_notify_connect(cons->index);
	
	cons->refcount++;
	
	/* Accept the connection */
	async_answer_0(iid, EOK);
	
	while (true) {
		async_serialize_end();
		callid = async_get_call(&call);
		async_serialize_start();
		
		arg1 = 0;
		arg2 = 0;
		arg3 = 0;
		
		switch (IPC_GET_IMETHOD(call)) {
		case IPC_M_PHONE_HUNGUP:
			cons->refcount--;
			if (cons->refcount == 0)
				gcons_notify_disconnect(cons->index);
			return;
		case VFS_OUT_READ:
			async_serialize_end();
			cons_read(cons, callid, &call);
			async_serialize_start();
			continue;
		case VFS_OUT_WRITE:
			async_serialize_end();
			cons_write(cons, callid, &call);
			async_serialize_start();
			continue;
		case VFS_OUT_SYNC:
			fb_pending_flush();
			if (cons == active_console) {
				async_req_0_0(fb_info.phone, FB_FLUSH);
				curs_goto(cons->scr.position_x, cons->scr.position_y);
			}
			break;
		case CONSOLE_CLEAR:
			/* Send message to fb */
			if (cons == active_console)
				async_msg_0(fb_info.phone, FB_CLEAR);
			
			screenbuffer_clear(&cons->scr);
			
			break;
		case CONSOLE_GOTO:
			screenbuffer_goto(&cons->scr,
			    IPC_GET_ARG1(call), IPC_GET_ARG2(call));
			if (cons == active_console)
				curs_goto(IPC_GET_ARG1(call),
				    IPC_GET_ARG2(call));
			break;
		case CONSOLE_GET_POS:
			arg1 = cons->scr.position_x;
			arg2 = cons->scr.position_y;
			break;
		case CONSOLE_GET_SIZE:
			arg1 = fb_info.cols;
			arg2 = fb_info.rows;
			break;
		case CONSOLE_GET_COLOR_CAP:
			rc = ccap_fb_to_con(fb_info.color_cap, &arg1);
			if (rc != EOK) {
				async_answer_0(callid, rc);
				continue;
			}
			break;
		case CONSOLE_SET_STYLE:
			fb_pending_flush();
			arg1 = IPC_GET_ARG1(call);
			screenbuffer_set_style(&cons->scr, arg1);
			if (cons == active_console)
				set_style(arg1);
			break;
		case CONSOLE_SET_COLOR:
			fb_pending_flush();
			arg1 = IPC_GET_ARG1(call);
			arg2 = IPC_GET_ARG2(call);
			arg3 = IPC_GET_ARG3(call);
			screenbuffer_set_color(&cons->scr, arg1, arg2, arg3);
			if (cons == active_console)
				set_color(arg1, arg2, arg3);
			break;
		case CONSOLE_SET_RGB_COLOR:
			fb_pending_flush();
			arg1 = IPC_GET_ARG1(call);
			arg2 = IPC_GET_ARG2(call);
			screenbuffer_set_rgb_color(&cons->scr, arg1, arg2);
			if (cons == active_console)
				set_rgb_color(arg1, arg2);
			break;
		case CONSOLE_CURSOR_VISIBILITY:
			fb_pending_flush();
			arg1 = IPC_GET_ARG1(call);
			cons->scr.is_cursor_visible = arg1;
			if (cons == active_console)
				curs_visibility(arg1);
			break;
		case CONSOLE_GET_EVENT:
			async_serialize_end();
			cons_get_event(cons, callid, &call);
			async_serialize_start();
			continue;
		case CONSOLE_KCON_ENABLE:
			change_console(kernel_console);
			break;
		}
		async_answer_3(callid, EOK, arg1, arg2, arg3);
	}
}

static void interrupt_received(ipc_callid_t callid, ipc_call_t *call)
{
	change_console(prev_console);
}

static int connect_keyboard(char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return fd;
	}
	
	int phone = fd_phone(fd);
	if (phone < 0) {
		printf(NAME ": Failed to connect to input device\n");
		return phone;
	}
	
	/* NB: The callback connection is slotted for removal */
	sysarg_t phonehash;
	sysarg_t taskhash;
	int rc = async_req_3_5(phone, IPC_M_CONNECT_TO_ME, SERVICE_CONSOLE,
	    0, 0, NULL, NULL, NULL, &taskhash, &phonehash);
	if (rc != EOK) {
		printf(NAME ": Failed to create callback from input device\n");
		return rc;
	}
	
	async_new_connection(taskhash, phonehash, 0, NULL, keyboard_events);

	printf(NAME ": we got a hit (new keyboard \"%s\").\n", path);

	return phone;
}

/** Try to connect to given keyboard, bypassing provided libc routines.
 *
 * @param devmap_path Path to keyboard without /dev prefix.
 * @return Phone or error code.
 */
static int connect_keyboard_bypass(char *devmap_path)
{
	int devmap_phone = async_connect_me_to_blocking(PHONE_NS,
	    SERVICE_DEVMAP, DEVMAP_CLIENT, 0);
	if (devmap_phone < 0) {
		return devmap_phone;
	}
	ipc_call_t answer;
	aid_t req = async_send_2(devmap_phone, DEVMAP_DEVICE_GET_HANDLE,
	    0, 0,  &answer);

	sysarg_t retval = async_data_write_start(devmap_phone,
	    devmap_path, str_size(devmap_path));
	if (retval != EOK) {
		async_wait_for(req, NULL);
		async_hangup(devmap_phone);
		return retval;
	}

	async_wait_for(req, &retval);

	if (retval != EOK) {
		async_hangup(devmap_phone);
		return retval;
	}

	devmap_handle_t handle = (devmap_handle_t) IPC_GET_ARG1(answer);

	async_hangup(devmap_phone);

	int phone = async_connect_me_to(PHONE_NS,
	    SERVICE_DEVMAP, DEVMAP_CONNECT_TO_DEVICE, handle);
	if (phone < 0) {
		return phone;
	}

	/* NB: The callback connection is slotted for removal */
	sysarg_t phonehash;
	sysarg_t taskhash;
	int rc = async_req_3_5(phone, IPC_M_CONNECT_TO_ME, SERVICE_CONSOLE,
	    0, 0, NULL, NULL, NULL, &taskhash, &phonehash);
	if (rc != EOK) {
		printf(NAME ": Failed to create callback from input device\n");
		return rc;
	}

	async_new_connection(taskhash, phonehash, 0, NULL, keyboard_events);

	printf(NAME ": we got a hit (new keyboard \"/dev/%s\").\n",
	    devmap_path);

	return phone;
}


static int check_new_keyboards(void *arg)
{
	char *class_name = (char *) arg;

	int index = 1;

	while (true) {
		async_usleep(1 * 500 * 1000);
		char *path;
		int rc = asprintf(&path, "class/%s\\%d", class_name, index);
		if (rc < 0) {
			continue;
		}
		rc = 0;
		rc = connect_keyboard_bypass(path);
		if (rc > 0) {
			/* We do not allow unplug. */
			index++;
		}

		free(path);
	}

	return EOK;
}


/** Start a fibril monitoring hot-plugged keyboards.
 */
static void check_new_keyboards_in_background()
{
	fid_t fid = fibril_create(check_new_keyboards, (void *)"keyboard");
	if (!fid) {
		printf(NAME ": failed to create hot-plug-watch fibril.\n");
		return;
	}
	fibril_add_ready(fid);
}

static bool console_init(char *input)
{
	/* Connect to input device */
	kbd_phone = connect_keyboard(input);
	if (kbd_phone < 0) {
		return false;
	}

	/* Connect to mouse device */
	mouse_phone = -1;
	int mouse_fd = open("/dev/hid_in/mouse", O_RDONLY);
	
	if (mouse_fd < 0) {
		printf(NAME ": Notice - failed opening %s\n", "/dev/hid_in/mouse");
		goto skip_mouse;
	}
	
	mouse_phone = fd_phone(mouse_fd);
	if (mouse_phone < 0) {
		printf(NAME ": Failed to connect to mouse device\n");
		goto skip_mouse;
	}
	
	if (async_connect_to_me(mouse_phone, SERVICE_CONSOLE, 0, 0, mouse_events)
	    != 0) {
		printf(NAME ": Failed to create callback from mouse device\n");
		mouse_phone = -1;
		goto skip_mouse;
	}
	
skip_mouse:
	
	/* Connect to framebuffer driver */
	fb_info.phone = service_connect_blocking(SERVICE_VIDEO, 0, 0);
	if (fb_info.phone < 0) {
		printf(NAME ": Failed to connect to video service\n");
		return -1;
	}
	
	/* Register driver */
	int rc = devmap_driver_register(NAME, client_connection);
	if (rc < 0) {
		printf(NAME ": Unable to register driver (%d)\n", rc);
		return false;
	}
	
	/* Initialize gcons */
	gcons_init(fb_info.phone);
	
	/* Synchronize, the gcons could put something in queue */
	async_req_0_0(fb_info.phone, FB_FLUSH);
	async_req_0_2(fb_info.phone, FB_GET_CSIZE, &fb_info.cols, &fb_info.rows);
	async_req_0_1(fb_info.phone, FB_GET_COLOR_CAP, &fb_info.color_cap);
	
	/* Set up shared memory buffer. */
	size_t ib_size = sizeof(keyfield_t) * fb_info.cols * fb_info.rows;
	interbuffer = as_get_mappable_page(ib_size);
	
	if (as_area_create(interbuffer, ib_size, AS_AREA_READ |
	    AS_AREA_WRITE | AS_AREA_CACHEABLE) != interbuffer)
		interbuffer = NULL;
	
	if (interbuffer) {
		if (async_share_out_start(fb_info.phone, interbuffer,
		    AS_AREA_READ) != EOK) {
			as_area_destroy(interbuffer);
			interbuffer = NULL;
		}
	}
	
	fb_pending.cnt = 0;
	
	/* Inititalize consoles */
	size_t i;
	for (i = 0; i < CONSOLE_COUNT; i++) {
		if (i != KERNEL_CONSOLE) {
			if (screenbuffer_init(&consoles[i].scr,
			    fb_info.cols, fb_info.rows) == NULL) {
				printf(NAME ": Unable to allocate screen buffer %zu\n", i);
				return false;
			}
			screenbuffer_clear(&consoles[i].scr);
			keybuffer_init(&consoles[i].keybuffer);
			consoles[i].index = i;
			consoles[i].refcount = 0;
			
			char vc[DEVMAP_NAME_MAXLEN + 1];
			snprintf(vc, DEVMAP_NAME_MAXLEN, "%s/vc%zu", NAMESPACE, i);
			
			if (devmap_device_register(vc, &consoles[i].devmap_handle) != EOK) {
				printf(NAME ": Unable to register device %s\n", vc);
				return false;
			}
		}
	}
	
	/* Disable kernel output to the console */
	__SYSCALL0(SYS_DEBUG_DISABLE_CONSOLE);
	
	/* Initialize the screen */
	async_serialize_start();
	gcons_redraw_console();
	set_style(STYLE_NORMAL);
	screen_clear();
	curs_goto(0, 0);
	curs_visibility(active_console->scr.is_cursor_visible);
	async_serialize_end();
	
	/* Receive kernel notifications */
	async_set_interrupt_received(interrupt_received);
	if (event_subscribe(EVENT_KCONSOLE, 0) != EOK)
		printf(NAME ": Error registering kconsole notifications\n");
	
	/* Start fibril for checking on hot-plugged keyboards. */
	check_new_keyboards_in_background();

	return true;
}

static void usage(void)
{
	printf("Usage: console <input>\n");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage();
		return -1;
	}
	
	printf(NAME ": HelenOS Console service\n");
	
	if (!console_init(argv[1]))
		return -1;

	printf(NAME ": Accepting connections\n");
	async_manager();
	
	return 0;
}

/** @}
 */
