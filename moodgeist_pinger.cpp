/********************************************************************************
 * moodgeist pinger for linux.                                                  *
 * small self-contained Skype plugin for http://www.moodgeist.com               *
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

#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include "skypeapi.h"

using namespace std;

class MoodgeistPinger : public SkypeAPI
{
	public:
		MoodgeistPinger();
		~MoodgeistPinger();

	private:
		virtual void handle_message(string message);
		long post_mood_for(string skypename, string mood, string lang);
		string escape_str(string in); ///< HTTP escape given string.

	private:
		struct UserData
		{
			UserData() : mood_text(), language() {}
			UserData(string mood, string lang) : mood_text(mood), language(lang) {}
			string mood_text;
			string language;
		};

		CURL *curl;
		map<string, UserData> users;
};

static bool starts_with(string in, string prefix)
{
	return in.substr(0, prefix.size()) == prefix;
}

MoodgeistPinger::MoodgeistPinger()
	: SkypeAPI("moodgeist_pinger", true)
	, curl(0)
{
	if(curl_global_init(CURL_GLOBAL_NOTHING))
		error("CURL global init failed.");
	curl = curl_easy_init();
	if(!curl)
		error("CURL easy init failed.");
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "http://www.moodgeist.com/do/ping/");
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Moodgeist/Linux");
}

MoodgeistPinger::~MoodgeistPinger()
{
	curl_easy_cleanup(curl);
}

//
// Escape arguments to HTTP POST.
//
string MoodgeistPinger::escape_str(string in)
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
long MoodgeistPinger::post_mood_for(string skypename, string mood, string lang)
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

void MoodgeistPinger::handle_message(string message)
{
	string out;
	if(check_sequence(message, out)) // response to our request
	{
		if(out == "PROTOCOL 5") // This will be received after we've just connected to Skype.
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
		if(starts_with(message, "CURRENTUSERHANDLE")) // FIXME should be handled in SkypeAPI
		{
			me = message.substr(message.find(" ")+1);
			wlog("Identified myself as %s\n", me.c_str());
		}
	}
}

//
// Entry point.
//
int main(int argc, char *argv[])
{
	MoodgeistPinger pinger;
	return pinger.exec();
}
