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
#include <iostream>
#include <sstream>
#include <string>
#include <map>

using namespace std;

// FSM states:
// Skype status: Present (SP), Not Present (SNP)
// Client status: Connected (CC), Not Connected (CNC) - aka Connecting, Denied (CD) - immediate quit
enum ClientState { CONNECTING, CONNECTED } clientState;

struct UserData
{
	UserData() : mood_text(), language() {}
	UserData(string mood, string lang) : mood_text(mood), language(lang) {}
	string mood_text;
	string language;
};

// Global variables (omgwtf!).
CURL *curl = 0;
string me;
bool running = true;
Display *xdisp = 0;
Atom atom1, atom2;
Window win = (Window)-1; // our message handling window
Window skype_win = (Window)-1;
map<Window, string> incoming_messages;
int sequence_number = 1;
map<string, UserData> users;

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
	{
		wlog("Sending X11 message failed with status %d\n", xerror);
		skypePresent(disp); // error might be due to Skype wandering off
	}

	return ok;
}

int send_next_message(string msg)
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

bool check_sequence(string msg, string &out)
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

bool starts_with(string in, string prefix)
{
	return in.substr(0, prefix.size()) == prefix;
}

//
// Send HTTP POST to moodgeist server.
//
long post_mood_for(string skypename, string mood, string lang)
{
	long response = 0;
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
// 	fprintf(stdout, "%s\n", message.c_str());

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
	}
	else
	{
		if(check_sequence(message, out)) // response to our request
		{
			if(out == "PROTOCOL 5")
			{
				send_next_message("SEARCH FRIENDS");
			}
			if(starts_with(out, "USERS"))
			{
				char buf[35]; // max skypename length is 32 bytes
				istringstream is(out.substr(out.find(" ")+1));
				is.setf(ios::skipws);
				while(!is.eof())
				{
					is.getline(buf, sizeof(buf), ',');
					send_next_message(string("GET USER ")+buf+" LANGUAGE");
					send_next_message(string("GET USER ")+buf+" MOOD_TEXT");
				}
			}
			if(starts_with(out, "USER "))
			{
				size_t lpos, mpos;
				size_t start = out.find(" ") + 1;
				size_t end = out.find(" ", start+1);

				string name = out.substr(start, end - start);

				if((lpos = out.find("LANGUAGE")) != string::npos)
				{
					users[name].language = out.substr(out.find(" ", lpos+1)+1, 2); // ISO language code
				}
				else
				if((mpos = out.find("MOOD_TEXT")) != string::npos)
				{
					users[name].mood_text = out.substr(out.find(" ", mpos+1)+1);

					if(!users[name].mood_text.empty())
						post_mood_for(name, users[name].mood_text, users[name].language);
				}

			}
		}
		else
		{
			if(starts_with(message, "CURRENTUSERHANDLE"))
			{
				me = message.substr(message.find(" ")+1);
				wlog("Identified myself as %s\n", me.c_str());
			}
		}
	}
}

// Run X event loop until we receive Ctrl-C or signal.
void run()
{
	XEvent ev;
	while(running) // outer loop handles fsm:SNP state
	{
		while(!skypePresent(xdisp)) // wait for fsm:SP
			sleep(1);

		clientState = CONNECTING;
		send_next_message("NAME moodgeist_pinger"); // kick protocol

		while(running && (skype_win != (Window)-1)) // inner loop handles fsm:SP state
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
	int fid = fork();

	if(fid == -1)
		error("Failed to fork.");

	if(fid != 0)
		exit(EXIT_SUCCESS); // started a child, now die off

	// child process
	// TODO: detach all console stuff

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
