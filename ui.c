/*-
 * Copyright (c) 2018 Stephen Hurd, W8BSD
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#define _WITH_GETLINE

#include <sys/ioctl.h>
#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <form.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "bsdtty.h"
#include "ui.h"

/* UI Stuff */
static WINDOW *status;
static WINDOW *status_title;
static WINDOW *rx;
static WINDOW *rx_title;
static WINDOW *tx;
static WINDOW *tx_title;
static int tx_width;
static int tx_height;
static bool reset_tuning = false;

static bool baudot_char(int ch, const void *ab);
static void capture_call(int y, int x);
static void do_endwin(void);
static char *escape_config(char *str);
static int find_field(const char *key);
static bool get_figs(chtype ch);
static void setup_windows(void);
static char *strip_spaces(char *str);
static void teardown_windows(void);
static void toggle_figs(int y, int x);
static void w_printf(WINDOW *win, const char *format, ...);
static char *unescape_config(char *str);

void
setup_curses(void)
{
	initscr();
	atexit(do_endwin);
	start_color();
	init_pair(1, COLOR_GREEN, COLOR_GREEN);
	init_pair(2, COLOR_YELLOW, COLOR_YELLOW);
	init_pair(3, COLOR_RED, COLOR_RED);
	raw();		// cbreak() leaves SIGINT working
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	curs_set(0);

	setup_windows();
	/*
	 * We need to fire on BUTTON1_PRESSED, not BUTTON1_CLICKED...
	 * curses will block in wgetch() while the button is down
	 * when using BUTTON1_CLICKED.  If we don't unmask
	 * BUTTON1_RELEASED along with BUTTON1_PRESSED, wgetch() will
	 * block indefinately after a clock(!)  We simply ignore the
	 * release event though.
	 * 
	 * As it happens, we need to capture BUTTON1_CLICKED as well.
	 * If we press and release "fast enough" it's a clicked event,
	 * not a press and release, even if clocks aren't captured and
	 * the interval is zero.
	 * 
	 * *sigh*
	 */
	mousemask(BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_RELEASED | BUTTON3_PRESSED | BUTTON3_CLICKED | BUTTON3_RELEASED, NULL);
	/*
	 * If we don't set mouseinterval to zero, we end up with delays
	 * in wgetch() as well.
	 */
	mouseinterval(0);
}

void
reset_tuning_aid(void)
{
	reset_tuning = true;
}

void
update_tuning_aid(double mark, double space)
{
	static double *buf = NULL;
	static int wsamp = 0;
	static int nsamp = -1;
	static double maxm = 0;
	static double maxs = 0;
	double mmult, smult;
	int madd, sadd;
	int phaseadj = 0;
	int y, x;
	chtype ch;
	int och;

	if (nsamp == -1)
		nsamp = settings.dsp_rate/((((double)settings.baud_numerator / settings.baud_denominator))*7.5);
	if (buf == NULL) {
		buf = malloc(sizeof(*buf) * nsamp * 2 + 1);
		if (buf == NULL)
			printf_errno("alocating %d bytes", sizeof(*buf) * nsamp * 2 + 1);
	}
	if (phaseadj < 0) {
		if (wsamp >= phaseadj)
			buf[(wsamp + phaseadj)*2] = mark;
		buf[wsamp*2+1] = space;
	}
	else {
		buf[wsamp*2] = mark;
		if (wsamp >= phaseadj)
			buf[(wsamp - phaseadj)*2+1] = space;
	}
	if (fabs(mark) > maxm)
		maxm = fabs(mark);
	if (fabs(space) > maxs)
		maxs = fabs(space);
	wsamp++;

	if (wsamp - phaseadj == nsamp) {
		mmult = maxm / (tx_width / 2 - 2);
		smult = maxs / (tx_height / 2 - 2);
		madd = tx_width / 2;
		sadd = tx_height / 2;
		werase(tx);
		if (mmult == 0 || smult == 0)
			return;
		for (wsamp = 0; wsamp < nsamp; wsamp++) {
			y = buf[wsamp*2+1] / smult + sadd;
			x = buf[wsamp*2] / mmult + madd;
			ch = mvwinch(tx, y, x);
			switch (ch & A_CHARTEXT) {
				case ' ':
					och = '.';
					break;
				case '.':
					och = '+';
					break;
				case ',':
					och = '-';
					break;
				case '-':
					och = '+';
					break;
				case '+':
					och = '#';
					break;
				case '#':
					och = '#';
					break;
				case ERR & A_CHARTEXT:
					printf_errno("no char %02x (%d) at %d %d %d %lf %lf", ch, ch, y, x, tx_width, maxm, buf[wsamp*2]);
				default:
					printf_errno("no char %02x (%d) at %d %d", ch, ch, y, x);
			}
			mvwaddch(tx, buf[wsamp*2+1] / smult + sadd, buf[wsamp*2] / mmult + madd, och);
		}
		wmove(tx, 0, 0);
		wrefresh(tx);
		if (reset_tuning)
			maxm = maxs = reset_tuning = 0;
		wsamp = 0;
	}
}

void
mark_tx_extent(bool start)
{
	w_printf(tx, "\n------- %s of transmission -------\n", start ? "Start" : "End");
}

int
get_input(void)
{
	int ret;
	MEVENT ev;

	ret = wgetch(tx);
	switch(ret) {
		case ERR:
			return -1;
		case KEY_RESIZE:
			teardown_windows();
			setup_windows();
			return 0;
		case KEY_BREAK:
			return 3;
		case KEY_F(1):
			return RTTY_FKEY(1);
		case KEY_F(2):
			return RTTY_FKEY(2);
		case KEY_F(3):
			return RTTY_FKEY(3);
		case KEY_F(4):
			return RTTY_FKEY(4);
		case KEY_F(5):
			return RTTY_FKEY(5);
		case KEY_F(6):
			return RTTY_FKEY(6);
		case KEY_F(7):
			return RTTY_FKEY(7);
		case KEY_F(8):
			return RTTY_FKEY(8);
		case KEY_F(9):
			return RTTY_FKEY(9);
		case KEY_F(10):
			return RTTY_FKEY(10);
		case KEY_BACKSPACE:
			return 8;
		case KEY_MOUSE:
			getmouse(&ev);
			if (ev.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED))
				toggle_figs(ev.y, ev.x);
			if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED))
				capture_call(ev.y, ev.x);
			return 0;
		default:
			return ret;
	}
}

void
write_tx(char ch)
{
	if (ch == '\r')
		waddch(tx, '\n');
	waddch(tx, ch);
	wrefresh(tx);
}

void
write_rx(char ch)
{
	int y, x;
	int my, mx;

	getyx(rx, y, x);
	getmaxyx(rx, my, mx);
	switch (ch) {
		case '\r':
			wmove(rx, y, 0);
			break;
		case '\n':
			if (y == my - 1)
				scroll(rx);
			else
				y++;
			wmove(rx, y, x);
			break;
		case 7:
			beep();
			break;
		case 5:
			// Ignore Who Are You?
			break;
		default:
			waddch(rx, ch);
			break;
	}
	wrefresh(rx);
}

static void
show_freq(void)
{
#ifdef WITH_OUTRIGGER
	static uint64_t lfreq = 0;
	uint64_t freq;
	enum rig_modes lmode = MODE_UNKNOWN;
	enum rig_modes mode;
	char fstr[32];
	int pos;

	if (rig) {
		freq = get_frequency(rig, VFO_UNKNOWN);
		if (freq) {
			sprintf(fstr, "%" PRIu64, freq);
			for (pos = strlen(fstr) - 3; pos > 0; pos -= 3) {
				memmove(&fstr[pos]+1, &fstr[pos], strlen(&fstr[pos])+1);
				fstr[pos] = '.';
			}
			if (lfreq != freq)
				mvwaddstr(status, 0, 19, fstr);
		}
		mode = get_mode(rig);
		switch(mode) {
			case MODE_UNKNOWN:
				fstr[0] = 0;
				break;
			case MODE_CW:
				strcpy(fstr, "CW");
				break;
			case MODE_CWN:
				strcpy(fstr, "CWN");
				break;
			case MODE_CWR:
				strcpy(fstr, "CWR");
				break;
			case MODE_CWRN:
				strcpy(fstr, "CWRN");
				break;
			case MODE_AM:
				strcpy(fstr, "AM");
				break;
			case MODE_LSB:
				strcpy(fstr, "LSB");
				break;
			case MODE_USB:
				strcpy(fstr, "USB");
				break;
			case MODE_FM:
				strcpy(fstr, "FM");
				break;
			case MODE_FMN:
				strcpy(fstr, "FMN");
				break;
			case MODE_FSK:
				strcpy(fstr, "FSK");
				break;
		}
		if (lmode != mode)
			mvwaddstr(status, 0, 44, fstr);
		if (lmode != mode || lfreq != freq)
			wrefresh(status);
		lmode = mode;
		lfreq = freq;
	}
#endif
}

bool
check_input(void)
{
	MEVENT ev;
	int ch;

	show_freq();
	ch = wgetch(rx);
	if (ch == KEY_MOUSE) {
		getmouse(&ev);
		if (ev.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED))
			toggle_figs(ev.y, ev.x);
		else if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED))
			capture_call(ev.y, ev.x);
		return check_input();	// TODO: Recusion!  Mah stack!
	}
	if (ch == 0x0c) {
		clear_rx_window();
		return false;
	}
	if (ch != ERR) {
		ungetch(ch);
		return true;
	}
	return false;
}

void
printf_errno(const char *format, ...)
{
	int eno = errno;
	char *msg = NULL;
	va_list ap;

	endwin();
	va_start(ap, format);
	if (vasprintf(&msg, format, ap) < 0)
		msg = NULL;
	va_end(ap);
	printf("%s (%s)\n", strerror(eno), msg ? msg : "");
	exit(EXIT_FAILURE);
}

static void
setup_windows(void)
{
	struct winsize ws;
	int datrows;
	int i;
	int y, x;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1)
		printf_errno("getting window size");
	if (ws.ws_row < 6)
		printf_errno("not enough rows");
	if (ws.ws_col < 10)
		printf_errno("not enough columns");
	datrows = ((ws.ws_row - 4) / 2);
	if ((status_title = newwin(1, ws.ws_col, 0, 0)) == NULL)
		printf_errno("creating status_title window");
	if ((status = newwin(1, ws.ws_col, 1, 0)) == NULL)
		printf_errno("creating status window");
	if ((rx_title = newwin(1, ws.ws_col, 2, 0)) == NULL)
		printf_errno("creating rx_title window");
	if ((rx = newwin(ws.ws_row - 4 - datrows, ws.ws_col, 3, 0)) == NULL)
		printf_errno("creating rx window");
	if ((tx_title = newwin(1, ws.ws_col, ws.ws_row - datrows - 1, 0)) == NULL)
		printf_errno("creating tx_title window");
	if ((tx = newwin(datrows, ws.ws_col, ws.ws_row - datrows, 0)) == NULL)
		printf_errno("creating tx window");
	tx_width = ws.ws_col;
	tx_height = datrows;
	scrollok(status_title, FALSE);
	scrollok(status, FALSE);
	scrollok(rx_title, FALSE);
	idlok(rx, TRUE);
	wsetscrreg(rx, 0, ws.ws_row - 4 - datrows - 1);
	scrollok(rx, TRUE);
	scrollok(tx_title, FALSE);
	idlok(tx, TRUE);
	wsetscrreg(tx, 0, datrows - 1);
	scrollok(tx, TRUE);
	wclear(status_title);
	wclear(status);
	wclear(rx_title);
	wclear(rx);
	wclear(tx_title);
	wclear(tx);
	wmove(status_title, 0, 0);
	wmove(status, 0, 0);
	wmove(rx_title, 0, 0);
	wmove(rx, 0, 0);
	wmove(tx_title, 0, 0);
	wmove(tx, 0, 0);
	for (i = 0; i < 3; i++) {
		waddch(status_title, ACS_HLINE);
		waddch(rx_title, ACS_HLINE);
		waddch(tx_title, ACS_HLINE);
	}
	waddstr(status_title, " Status ");
	getyx(status_title, y, x);
	for (i = x; i < ws.ws_col; i++)
		waddch(status_title, ACS_HLINE);
	waddstr(rx_title, " RX ");
	getyx(rx_title, y, x);
	for (i = x; i < ws.ws_col; i++)
		waddch(rx_title, ACS_HLINE);
	waddstr(tx_title, " TX ");
	getyx(tx_title, y, x);
	for (i = x; i < ws.ws_col; i++)
		waddch(tx_title, ACS_HLINE);
	wrefresh(status_title);
	wrefresh(status);
	wrefresh(rx_title);
	wrefresh(rx);
	wrefresh(tx_title);
	wrefresh(tx);
	if (settings.afsk)
		wtimeout(tx, 0);
	else
		wtimeout(tx, -1);
	wtimeout(rx, 0);
	keypad(rx, TRUE);
	keypad(tx, TRUE);
}

static void
teardown_windows(void)
{
	delwin(tx);
	delwin(tx_title);
	delwin(rx);
	delwin(rx_title);
	delwin(status);
	delwin(status_title);
}

static void
w_printf(WINDOW *win, const char *format, ...)
{
	char *msg = NULL;
	va_list ap;

	va_start(ap, format);
	if (vasprintf(&msg, format, ap) < 0)
		msg = NULL;
	va_end(ap);
	waddstr(win, msg);
	wrefresh(win);
}

static void
do_endwin(void)
{
	endwin();
}

void
show_reverse(bool rev)
{
	mvwaddstr(status, 0, 1, rev ? "REV" : "   ");
	wrefresh(status);
}

struct field_info {
	const char *name;
	const char *key;
	enum field_type {
		STYPE_STRING,
		STYPE_BAUDOT,
		STYPE_DOUBLE,
		STYPE_INT,
		STYPE_BOOL
	} type;
	void *ptr;
} fields[] = {
	{
		.name = "Log file name",
		.key = "logfile",
		.type = STYPE_STRING,
		.ptr = ((char *)(&settings)) + offsetof(struct bt_settings, log_name)
	},
	{
		.name = "TTY device name",
		.key = "ttydevice",
		.type = STYPE_STRING,
		.ptr = ((char *)(&settings)) + offsetof(struct bt_settings, tty_name)
	},
	{
		.name = "DSP device name",
		.key = "dspdevice",
		.type = STYPE_STRING,
		.ptr = ((char *)(&settings)) + offsetof(struct bt_settings, dsp_name)
	},
	{
		.name = "Bandpass filter Q",
		.key = "bandpassq",
		.type = STYPE_DOUBLE,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, bp_filter_q)
	},
	{
		.name = "Lowpass filter Q",
		.key = "lowpassq",
		.type = STYPE_DOUBLE,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, lp_filter_q)
	},
	{
		.name = "Mark frequency",
		.key = "markfreq",
		.type = STYPE_DOUBLE,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, mark_freq)
	},
	{
		.name = "Space frequency",
		.key = "spacefreq",
		.type = STYPE_DOUBLE,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, space_freq)
	},
	{
		.name = "DSP sample rate",
		.key = "dsprate",
		.type = STYPE_INT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, dsp_rate)
	},
	{
		.name = "Baud numerator",
		.key = "baudnumerator",
		.type = STYPE_INT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, baud_numerator)
	},
	{
		.name = "Baud denominator",
		.key = "bauddenominator",
		.type = STYPE_INT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, baud_denominator)
	},
	{
		.name = "F1 macro",
		.key = "f1",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 0
	},
	{
		.name = "F2 macro",
		.key = "f2",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 1
	},
	{
		.name = "F3 macro",
		.key = "f3",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 2
	},
	{
		.name = "F4 macro",
		.key = "f4",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 3
	},
	{
		.name = "F5 macro",
		.key = "f5",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 4
	},
	{
		.name = "F6 macro",
		.key = "f6",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 5
	},
	{
		.name = "F7 macro",
		.key = "f7",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 6
	},
	{
		.name = "F8 macro",
		.key = "f8",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 7
	},
	{
		.name = "F9 macro",
		.key = "f9",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 8
	},
	{
		.name = "F10 macro",
		.key = "f10",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, macros) + sizeof(settings.macros[0]) * 9
	},
	{
		.name = "Character set",
		.key = "charset",
		.type = STYPE_INT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, charset)
	},
	{
		.name = "AFSK mode",
		.key = "afsk",
		.type = STYPE_BOOL,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, afsk)
	},
	{
		.name = "Callsign",
		.key = "callsign",
		.type = STYPE_BAUDOT,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, callsign)
	},
#ifdef WITH_OUTRIGGER
	{
		.name = "Outrigger PTT",
		.key = "outriggerptt",
		.type = STYPE_BOOL,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, or_ptt)
	},
	{
		.name = "Outrigger rig",
		.key = "outriggerrig",
		.type = STYPE_STRING,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, or_rig)
	},
	{
		.name = "Outrigger device",
		.key = "outriggerdev",
		.type = STYPE_STRING,
		.ptr = (char *)(&settings) + offsetof(struct bt_settings, or_dev)
	},
#endif
};
#define NUM_FIELDS (sizeof(fields) / sizeof(fields[0]))

void
change_settings(void)
{
	int i;
	FIELD *field[NUM_FIELDS + 1];
	FORM *frm;
	int ch;
	char cv[20];
	size_t end;
	FIELDTYPE *baudot;
	FILE *config;
	char *fname;
	char *bd;

	baudot = new_fieldtype(NULL, baudot_char);
	clear();
	refresh();
	for (i = 0; i < NUM_FIELDS; i++) {
		field[i] = new_field(1, (fields[i].type == STYPE_STRING || fields[i].type == STYPE_BAUDOT) ? tx_width - 26 : 20, 1+i, 20, 0, 0);
		set_field_back(field[i], A_UNDERLINE);
		field_opts_off(field[i], O_AUTOSKIP);
		switch (fields[i].type) {
			case STYPE_STRING:
				cv[0] = 0;
				field_opts_off(field[i], O_STATIC);
				if (*(char **)fields[i].ptr)
					set_field_buffer(field[i], 0, *(char **)fields[i].ptr);
				else
					set_field_buffer(field[i], 0, "");
				break;
			case STYPE_BAUDOT:
				set_field_type(field[i], baudot, 0, 1, 0);
				cv[0] = 0;
				field_opts_off(field[i], O_STATIC);
				if (*(char **)fields[i].ptr) {
					bd = strdup(*(char **)fields[i].ptr);
					if (bd == NULL)
						printf_errno("strup()ing baudot string");
					escape_config(bd);
					set_field_buffer(field[i], 0, bd);
					free(bd);
				}
				else
					set_field_buffer(field[i], 0, "");
				break;
			case STYPE_DOUBLE:
				set_field_type(field[i], TYPE_NUMERIC, 0, 1, 0);
				snprintf(cv, sizeof(cv), "%lf", *(double *)fields[i].ptr);
				end = strlen(cv);
				for (end--; end > 0 && cv[end] == '0'; end--)
					cv[end] = 0;
				if (cv[end] == '.')
					cv[end] = 0;
				set_field_buffer(field[i], 0, cv);
				break;
			case STYPE_INT:
				set_field_type(field[i], TYPE_INTEGER, 0, 1, 0);
				snprintf(cv, sizeof(cv), "%d", *(int *)fields[i].ptr);
				set_field_buffer(field[i], 0, cv);
				break;
			case STYPE_BOOL:
				set_field_type(field[i], TYPE_INTEGER, 0, 0, 1);
				snprintf(cv, sizeof(cv), "%d", *(bool *)fields[i].ptr);
				set_field_buffer(field[i], 0, cv);
				break;
		}
	}
	field[NUM_FIELDS] = NULL;

	frm = new_form(field);
	post_form(frm);
	for (i = 0; i < NUM_FIELDS; i++) {
		mvprintw(i+1, 1, fields[i].name);
	}

	form_driver(frm, REQ_END_LINE);
	refresh();
	while ((ch = getch()) != ERR) {
		switch (ch) {
			case 3:
			case KEY_BREAK:
				goto done;
			case '\r':
			case KEY_ENTER:
				goto done;
			case KEY_DOWN:
				form_driver(frm, REQ_NEXT_FIELD);
				form_driver(frm, REQ_END_LINE);
				break;
			case KEY_UP:
				form_driver(frm, REQ_PREV_FIELD);
				form_driver(frm, REQ_END_LINE);
				break;
			case KEY_LEFT:
				form_driver(frm, REQ_PREV_CHAR);
				break;
			case KEY_RIGHT:
				form_driver(frm, REQ_NEXT_CHAR);
				break;
			case KEY_HOME:
				form_driver(frm, REQ_BEG_FIELD);
				break;
			case KEY_END:
				form_driver(frm, REQ_END_FIELD);
				break;
			case 8:
			case 127:
			case KEY_BACKSPACE:
				form_driver(frm, REQ_DEL_PREV);
				break;
			case KEY_DC:
				form_driver(frm, REQ_DEL_CHAR);
				break;
			default:
				form_driver(frm, ch);
				break;
		}
	}
done:

	unpost_form(frm);

	if (ch == '\r' || ch == KEY_ENTER) {
		if (!getenv("HOME"))
			printf_errno("HOME not set");
		if (asprintf(&fname, "%s/.bsdtty", getenv("HOME")) < 0)
			printf_errno("unable to create filename");
		config = fopen(fname, "w");
		if (config == NULL)
			printf_errno("error opening \"%s\"", fname);
		for (i = 0; i < NUM_FIELDS; i++) {
			switch(fields[i].type) {
				case STYPE_BAUDOT:
					fprintf(config, "%s=%s\n", fields[i].key, escape_config(unescape_config(field_buffer(field[i], 0))));
					break;
				default:
					fprintf(config, "%s=%s\n", fields[i].key, strip_spaces(field_buffer(field[i], 0)));
					break;
			}
		}
		fclose(config);
	}

	free_form(frm);

	for (i = 0; i < NUM_FIELDS; i++) {
		free_field(field[i]);
	}
	free_fieldtype(baudot);

	touchwin(status_title);
	touchwin(status);
	touchwin(rx_title);
	touchwin(rx);
	touchwin(tx_title);
	touchwin(tx);
	wrefresh(status_title);
	wrefresh(status);
	wrefresh(rx_title);
	wrefresh(rx);
	wrefresh(tx_title);
	wrefresh(tx);
}

static bool
baudot_char(int ch, const void *ab)
{
	if (asc2baudot(ch, false))
		return true;
	if (ch == '~')
		return true;
	if (ch == '_')
		return true;
	return false;
}

static char *
strip_spaces(char *str)
{
	char *ch;

	for (ch = strchr(str, 0) - 1; ch >= str && *ch == ' '; ch--)
		*ch = 0;
	return str;
}

static char *
escape_config(char *str)
{
	char *ch;

	for (ch = str; *ch; ch++) {
		*ch = toupper(*ch);
		switch (*ch) {
			case '\t':
				*ch = '~';
				ch[1] = 0;
				break;
			case ' ':
				if (ch[1] == 0)
					*ch = '_';
				break;
		}
	}

	return str;
}

static char *
unescape_config(char *str)
{
	char *ch;

	strip_spaces(str);
	for (ch = strchr(str, 0) - 1; ch >= str && *ch == ' '; ch--)
		*ch = 0;

	for (ch = str; *ch; ch++) {
		*ch = toupper(*ch);
		switch (*ch) {
			case '~':
				*ch = '\t';
				ch[1] = 0;
				break;
			case '_':
				*ch = ' ';
				break;
		}
	}

	return str;
}

void
load_config(void)
{
	FILE *config;
	char *fname;
	char *line = NULL;
	size_t lcapp = 0;
	char *ch;
	int field;

	if (!getenv("HOME"))
		printf_errno("HOME not set");
	if (asprintf(&fname, "%s/.bsdtty", getenv("HOME")) < 0)
		printf_errno("unable to create filename");
	config = fopen(fname, "r");
	if (config == NULL)
		return;
	while (getline(&line, &lcapp, config) != -1) {
		if (line[0] == '#')
			continue;
		for (ch = strchr(line, 0) - 1; ch > line && *ch == '\n'; ch--)
			*ch = 0;
		ch = strchr(line, '=');
		if (ch == NULL)
			continue;
		*ch = 0;
		ch++;
		field = find_field(line);
		if (field == -1)
			continue;
		switch(fields[field].type) {
			case STYPE_STRING:
				if (*(char **)fields[field].ptr)
					free(*(char **)fields[field].ptr);
				*(char **)fields[field].ptr = strdup(ch);
				break;
			case STYPE_BAUDOT:
				if (*(char **)fields[field].ptr)
					free(*(char **)fields[field].ptr);
				*(char **)fields[field].ptr = strdup(ch);
				unescape_config(*(char **)fields[field].ptr);
				break;
			case STYPE_DOUBLE:
				*(double *)fields[field].ptr = strtod(ch, NULL);
				break;
			case STYPE_INT:
				*(int *)fields[field].ptr = strtoi(ch, NULL, 10);
				break;
			case STYPE_BOOL:
				*(bool *)fields[field].ptr = strtoi(ch, NULL, 10);
				break;
		}
	}
	fclose(config);
}

static int
find_field(const char *key)
{
	int i;

	for (i = 0; i < NUM_FIELDS; i++) {
		if (strcasecmp(fields[i].key, key) == 0)
			return i;
	}

	return -1;
}

void
display_charset(const char *name)
{
	char padded[12];

	snprintf(padded, sizeof(padded), "%-11s", name);
	mvwaddstr(status, 0, 6, padded);
	wrefresh(status);
}

void
audio_meter(int16_t envelope)
{
	int i = 0;
	int blocks = envelope / (INT16_MAX / 48);

	if (blocks > 16)
		blocks = 16;
	mvwaddstr(status, 0, tx_width - 16, "                ");
	wmove(status, 0, tx_width - 16);
	wcolor_set(status, 1, NULL);
	wattron(status, A_BOLD);
	for (i = 0; i < blocks; i++) {
		if (i == 12)
			wcolor_set(status, 2, NULL);
		else if (i == 14)
			wcolor_set(status, 3, NULL);
		waddch(status, ACS_BLOCK);
	}
	wcolor_set(status, 0, NULL);
	wattroff(status, A_BOLD);
	wrefresh(status);
}

static bool
get_figs(chtype ch)
{
	char bch;

	bch = asc2baudot(ch & A_CHARTEXT, false);
	return bch & 0x20;
}

static bool
can_print_inverse(chtype ch)
{
	bool figs;
	char bch, ach;

	ch &= A_CHARTEXT;
	if (ch == 0)
		return false;
	bch = asc2baudot(ch, false);
	if (bch == 0)
		return false;
	figs = bch & 0x20;
	bch &= 0x1f;
	ach = baudot2asc(bch, !figs);
	return isprint(ach);
}

static bool is_call_char(chtype ch)
{
	ch &= A_CHARTEXT;
	if (ch >= 'A' && ch <= 'Z')
		return true;
	if (ch >= '0' && ch <= '9')
		return true;
	if (ch == '/')
		return true;
	return false;
}

static void
capture_call(int y, int x)
{
	int sx, sy;
	int width, height;
	int fx, fy;
	chtype ch;
	int iy, ix;
	char captured[15];
	char *c = captured;
	char *e;

	getyx(rx, iy, ix);
	getbegyx(rx, sy, sx);
	getmaxyx(rx, height, width);

	if (y < sx || x < sx || y >= sy + height || x >= sx + width)
		return;
	/* From x/y, move left until the edge, or a non-call char */
	fy = y - sy;
	ch = mvwinch(rx, fy , x - sx);
	if (!is_call_char(ch))
		goto done;
	for (fx = x - sx - 1; fx >= 0; fx--) {
		ch = mvwinch(rx, fy , fx);
		if (!is_call_char(ch)) {
			fx++;
			break;
		}
	}
	if (fx == -1)
		fx = 0;
	/* Now, from fy/fx move left toggling shift until more space */
	for (; fx < width; fx++) {
		ch = mvwinch(rx, fy , fx);
		if (is_call_char(ch)) {
			*c++ = ch & A_CHARTEXT;
			if (c == &captured[sizeof(captured)-1])
				goto done;
		}
		else
			break;
	}
	e = c;
	while(c < &captured[sizeof(captured)-1])
		*(c++) = ' ';
	*c = 0;
	mvwaddstr(status, 0, 50, captured);
	*e = 0;
	captured_callsign(captured);
	wrefresh(status);

done:
	wmove(rx, iy, ix);
	wrefresh(rx);
}

static void
toggle_figs(int y, int x)
{
	int sx, sy;
	int width, height;
	bool figs = false;
	int fx, fy;
	chtype ch;
	char bch;
	char ach;
	int iy, ix;

	getyx(rx, iy, ix);
	getbegyx(rx, sy, sx);
	getmaxyx(rx, height, width);

	if (y < sx || x < sx || y >= sy + height || x >= sx + width)
		return;
	/* From x/y, move left until the edge, whitespace, or figs changes */
	fy = y - sy;
	ch = mvwinch(rx, fy , x - sx);
	figs = get_figs(ch);
	for (fx = x - sx; fx >= 0; fx--) {
		ch = mvwinch(rx, fy , fx);
		ch &= A_CHARTEXT;
		if (ch == 0 || isspace(ch) || get_figs(ch) != figs || !can_print_inverse(ch)) {
			if (fx < x - sx)
				fx++;
			break;
		}
	}
	if (fx == -1)
		fx = 0;
	/* Now, from fy/fx move left toggling shift until more space */
	for (; fx < width; fx++) {
		ch = mvwinch(rx, fy , fx);
		if (get_figs(ch) != figs)
			break;
		if (!can_print_inverse(ch))
			break;
		ch &= A_CHARTEXT;
		if (ch == 0 || isspace(ch))
			break;
		bch = asc2baudot(ch, figs);
		if (bch == 0)
			break;
		bch &= 0x1f;
		ach = baudot2asc(bch, !figs);
		if (!isprint(ach))
			break;
		mvwaddch(rx, fy, fx, ach);
	}
	wmove(rx, iy, ix);
	wrefresh(rx);
}

void
clear_rx_window(void)
{
	wclear(rx);
	wmove(rx, 0, 0);
	wrefresh(rx);
}
