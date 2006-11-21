/*******************************************
 * moodgeist pinger for linux.             *
 * written by berkus <berkus@madfire.net>  *
 * distributed under terms of MIT license. *
 *******************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <curl/curl.h>
#include <string>
#include <map>

using namespace std;

// Global variables (omgwtf!).
CURL *curl = 0;
string me;
bool running = true;
Display *xdisp = 0;
Atom atom1, atom2;
Window win = (Window)-1; // our message handling window
Window skype_win = (Window)-1;
std::map<Window, std::string> incoming_messages;

// Write to log file only if it already exists.
void wlog(const char *format, ...)
{
	static string log_filename;
	struct stat statbuf;
	va_list ap;

	if(log_filename.empty())
	{
		log_filename.append(getenv("HOME"));
		log_filename.append("/.moodgeist/log");
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

void error(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	wlog("%s\n", msg);
	exit(EXIT_FAILURE);
}

//
// Detect Skype presence.
//
int skypePresent(Display *disp)
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

int xerrhandler(Display *dpy, XErrorEvent *err)
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

int send_message(Window w_P, const char *message_P, Display *disp, Window handle_P)
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
		wlog("Sending X11 message failed with status %d\n", xerror);

	return ok;
}

//
// Escape arguments to HTTP POST.
//
string escape_str(string in)
{
	char *out = curl_easy_escape(curl, in.c_str(), in.size());
	if(!out)
	{
		wlog("Unable to escape string %s.\n", in.c_str());
		return string();
	}
	string str(out); // makes a copy
	curl_free(out);
	return str;
}

//
// Send HTTP POST to moodgeist server.
//
long post_mood_for(string skypename, string mood, string lang)
{
	long response;
	string fld;

	fld.append("protocol=1&skypename=");
	fld.append(escape_str(skypename));
	fld.append("&mood_text=");
	fld.append(escape_str(mood));
	fld.append("&poster=");
	fld.append(escape_str(me));
	fld.append("&skypename_language=");
	fld.append(escape_str(lang));

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fld.c_str());
	if(curl_easy_perform(curl))
		wlog("Error sending POST with %s\n", fld.c_str());
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response); // return 0 if ok
	wlog("POST %s response code %ld.\n", fld.c_str(), response);

	return response;
}

//
// Event Loop.
//
void handle_message(Window wid, string message)
{
}

// Run X event loop until we receive Ctrl-C or signal.
void run()
{
	XEvent ev;
	while(running)
	{
		XNextEvent(xdisp, &ev);
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

// Quit upon receiving any signal.
void sighandler()
{
	running = false;
}

//
// Entry point.
//
int main(int argc, char *argv[])
{
	xdisp = XOpenDisplay(getenv("DISPLAY"));
	if(!xdisp)
		error("Cannot open display.");
	Window root = DefaultRootWindow(xdisp);
	win = XCreateSimpleWindow(xdisp, root, 0, 0, 1, 1,
		0, BlackPixel(xdisp, DefaultScreen(xdisp)),
		BlackPixel(xdisp, DefaultScreen(xdisp)));
	atom1 = XInternAtom(xdisp, "SKYPECONTROLAPI_MESSAGE_BEGIN", False);
	atom2 = XInternAtom(xdisp, "SKYPECONTROLAPI_MESSAGE", False);

	if(curl_global_init(CURL_GLOBAL_NOTHING))
		error("CURL global init failed.");
	curl = curl_easy_init();
	if(!curl)
		error("CURL easy init failed.");
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "http://www.moodgeist.com/do/ping/");
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Moodgeist/Linux");

	// TODO install sig handler

	run();

	curl_easy_cleanup(curl);
	XDestroyWindow(xdisp, win);
	XCloseDisplay(xdisp);
	return EXIT_SUCCESS;
}