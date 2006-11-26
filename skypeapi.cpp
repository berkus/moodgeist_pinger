/********************************************************************************
 * Simple Skype X11 API plugin framework.                                       *
 * written by berkus <berkus@madfire.net>                                       *
 *                                                                              *
 * Copyright (c) 2006 Stanislav Karchebny                                       *
 *                                                                              *
 * Permission is hereby granted, free of charge, to any person obtaining        *
 * a copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation    *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
 * and/or sell copies of the Software, and to permit persons to whom            *
 * the Software is furnished to do so, subject to the following conditions:     *
 *                                                                              *
 * The above copyright notice and this permission notice shall be included      *
 * in all copies or substantial portions of the Software.                       *
 *                                                                              *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,              *
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES              *
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.    *
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,  *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,                *
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE   *
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                *
 ********************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include "skypeapi.h"

using namespace std;

SkypeAPI::SkypeAPI(std::string plugin_name, bool daemonize)
	: clientState(CONNECTING)
	, name(plugin_name)
	, running(true)
	, xdisp(0)
	, win((Window)-1)
	, skype_win((Window)-1)
	, sequence_number(1)
{
	if(daemonize)
	{
		int fid = fork();

		if(fid == -1)
			error("Failed to fork.");

		if(fid != 0)
			exit(EXIT_SUCCESS); // started a child, now die off

		// child process
		// TODO: detach all console stuff
	}

	xdisp = XOpenDisplay(getenv("DISPLAY"));
	if(!xdisp)
		error("Cannot open display.");
	Window root = DefaultRootWindow(xdisp);
	win = XCreateSimpleWindow(xdisp, root, 0, 0, 1, 1,
		0, BlackPixel(xdisp, DefaultScreen(xdisp)),
		BlackPixel(xdisp, DefaultScreen(xdisp)));
	atom1 = XInternAtom(xdisp, "SKYPECONTROLAPI_MESSAGE_BEGIN", False);
	atom2 = XInternAtom(xdisp, "SKYPECONTROLAPI_MESSAGE", False);

	// TODO install sig handler
}

SkypeAPI::~SkypeAPI()
{
	XDestroyWindow(xdisp, win);
	XCloseDisplay(xdisp);
}

void SkypeAPI::error(const char *msg)
{
	wlog("%s\n", msg);
	exit(EXIT_FAILURE);
}

// Run X event loop until we receive Ctrl-C or signal.
int SkypeAPI::exec()
{
	XEvent ev;
	while(running) // outer loop handles fsm:SNP state
	{
		while(!skype_present(xdisp)) // wait for fsm:SP
			sleep(1);

		clientState = CONNECTING;
		send_next_message(string("NAME ") + name); // kick protocol

		while(running && (skype_win != (Window)-1)) // inner loop handles fsm:SP state
		{
			XNextEvent(xdisp, &ev);

			if(process_x_events(&ev))
				continue;

			if(ev.type != ClientMessage || ev.xclient.format != 8)
				continue;

			// These are not the atoms you are looking for...
			if(ev.xclient.message_type != atom1 && ev.xclient.message_type != atom2)
				continue;

			char buf[21]; // can't be longer
			int i;
			for(i = 0; i < 20 && ev.xclient.data.b[i] != '\0'; ++i)
				buf[i] = ev.xclient.data.b[i];

			buf[i] = '\0';

			if(incoming_messages.find(ev.xclient.window) != incoming_messages.end())
			{
				if(ev.xclient.message_type == atom1 && atom1 != atom2)
					// two different messages on the same window at the same time shouldn't happen anyway
					incoming_messages[ev.xclient.window] = string();

				incoming_messages[ev.xclient.window].append(buf);
			}
			else
			{
				if(ev.xclient.message_type == atom2 && atom1 != atom2)
					continue; // middle of message, but we don't have the beginning, ignore

				incoming_messages[ev.xclient.window] = buf;
			}

			if(i < 20) // last message fragment
			{
				handle_message(ev.xclient.window, incoming_messages[ev.xclient.window]);
				incoming_messages.erase(ev.xclient.window);
			}
		}
	}

	return EXIT_SUCCESS;
}

// Quit upon receiving any signal.
void sighandler()
{
///	running = false;
}

//
// Detect Skype presence.
//
int SkypeAPI::skype_present(Display *disp)
{
	Atom skype_inst = XInternAtom(disp, "_SKYPE_INSTANCE", True);

	Atom type_ret;
	int format_ret;
	unsigned long nitems_ret;
	unsigned long bytes_after_ret;
	unsigned char *prop;
	int status;

	status = XGetWindowProperty(disp, DefaultRootWindow(disp), skype_inst, 0, 1, False, XA_WINDOW, &type_ret, &format_ret, &nitems_ret, &bytes_after_ret, &prop);

	// sanity check
	if(status != Success || format_ret != 32 || nitems_ret != 1)
	{
		skype_win = (Window)-1;
		wlog("Skype instance not found\n");
		return 0;
	}

	skype_win = * (const unsigned long *) prop & 0xffffffff;
	wlog("Skype instance found with id #%x\n", skype_win);
	return 1;
}

//
// Send X11 ClientMessage to Skype.
//
static XErrorHandler old_handler = 0;
static int xerror = 0;

static int xerrhandler(Display *dpy, XErrorEvent *err)
{
   xerror = err->error_code;
   return 0; // ignore the error
}

static void trap_errors()
{
   xerror = 0;
   old_handler = XSetErrorHandler(xerrhandler);
}

static int untrap_errors()
{
   XSetErrorHandler(old_handler);
   return (xerror != BadValue) && (xerror != BadWindow);
}

int SkypeAPI::send_message(Window w_P, const char *message_P, Display *disp, Window handle_P)
{
	unsigned int pos = 0;
	unsigned int len = strlen(message_P);
	XEvent e;
	int ok;

	memset(&e, 0, sizeof(e));
	e.xclient.type = ClientMessage;
	e.xclient.message_type = atom1; // leading message
	e.xclient.display = disp;
	e.xclient.window = handle_P;
	e.xclient.format = 8;

	trap_errors();
	do
	{
		unsigned int i;
		for(i = 0; i < 20 && i + pos <= len; ++i)
			e.xclient.data.b[i] = message_P[i + pos];
		XSendEvent(disp, w_P, False, 0, &e);

		e.xclient.message_type = atom2; // following messages
		pos += i;
	} while(pos <= len);

	XSync(disp, False);
	ok = untrap_errors();

	if(!ok)
	{
		wlog("Sending X11 message failed with status %d\n", xerror);
		skype_present(disp); // error might be due to Skype wandering off
	}

	return ok;
}

int SkypeAPI::send_next_message(string msg)
{
	string message("#");
	ostringstream o;
	o << sequence_number;
	message.append(o.str());
	message.append(" ");
	message.append(msg);

	sequence_number++;
	return send_message(skype_win, message.c_str(), xdisp, win);
}

bool SkypeAPI::check_sequence(string msg, string &out)
{
	if(msg[0] != '#')
	{
		out = msg;
		return false; // we didn't ask for this, pass thru
	}

	int n;
	string sub(msg.substr(1, msg.find(" ")));
	istringstream i(sub);
	i >> n;
	if(n > sequence_number-1)
		exit(EXIT_FAILURE); // we're out of sync, better die

	string final(msg.substr(msg.find(" ")+1));
	out = final;
	return true;
}

void SkypeAPI::handle_message(Window wid, string message)
{
	string out;
	if(clientState == CONNECTING)
	{
		if(check_sequence(message, out))
		{
			if(out == "OK")
			{
				clientState = CONNECTED;
				send_next_message("PROTOCOL 5");
			}
			else
				running = false; // fsm:CD, die off at once
		}
		else
		{
			wlog("Invalid Skype response in CONNECTING phase: %s\n", message.c_str());
			// FIXME die here?
		}
	}
	else
	{
		handle_message(message);
	}
}

bool SkypeAPI::process_x_events(XEvent *)
{
	return false;
}

// Write to log file only if it already exists.
void SkypeAPI::wlog(const char *format, ...)
{
	static string log_filename;
	struct stat statbuf;
	va_list ap;

	if(log_filename.empty())
	{
		log_filename.append(getenv("HOME"));
		log_filename.append(string("/.Skype/Plugins/")+name+"/log");
	}

	if(!stat(log_filename.c_str(), &statbuf))
	{
		if(S_ISREG(statbuf.st_mode))
		{
			FILE *fp = fopen(log_filename.c_str(), "a");
			va_start(ap, format);
			if(fp)
			{
				char timebuf[30];
				time_t t;
				tm *tmp;
				t = time(NULL);
				tmp = localtime(&t);
				if(tmp)
				{
					if(strftime(timebuf, sizeof(timebuf), "%d %b %Y %H:%M:%S", tmp))
						fprintf(fp, "[%s] ", timebuf);
				}
				vfprintf(fp, format, ap);
				fclose(fp);
			}
			va_end(ap);
		}
	}
	else
	{
		if(errno != ENOENT)
			fprintf(stderr, "Some filesystem error while checking log file. Ignored.\n");
	}
}
