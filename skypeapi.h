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
#ifndef INCLUDED__SKYPEAPI_H
#define INCLUDED__SKYPEAPI_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string>
#include <map>

class SkypeAPI
{
	public:
		// FSM states:
		// Skype status: Present (SP), Not Present (SNP)
		// Client status: Connected (CC), Not Connected (CNC) - aka Connecting, Denied (CD) - immediate quit
		enum ClientState { CONNECTING, CONNECTED };

		SkypeAPI(std::string plugin_name, bool daemonize = false);
		virtual ~SkypeAPI();

		int exec(); ///< Run plugin event loop, return exit code.

		virtual void handle_message(std::string message) = 0; ///< Override this to receive Skype messages.
		virtual bool process_x_events(XEvent *ev); ///< Override this to take control of X events processing, return @c true if you handled the event yourself (it will not reach handle_message() processing at all).
		int send_next_message(std::string msg); ///< Call this to send next numbered message to Skype.
		bool check_sequence(std::string msg, std::string &out); ///< Return @c true if received message is response to our request.

		inline ClientState client_state() { return clientState; }

		void wlog(const char *format, ...); ///< Write message to log.
		void error(const char *msg); ///< Write message to log and exit.

	protected:
		virtual void handle_message(Window wid, std::string message); ///< Main message handler, usually needs no overrides.

		int skype_present(Display *disp);
		int send_message(Window w_P, const char *message_P, Display *disp, Window handle_P);

		ClientState clientState;
		std::string name;
		std::string me;
		bool running;
		Display *xdisp;
		Atom atom1, atom2;
		Window win; // our message handling window
		Window skype_win;
		std::map<Window, std::string> incoming_messages;
		int sequence_number;
};

#endif // INCLUDED__SKYPEAPI_H
