moodgeist_pinger: moodgeist_pinger.cpp skypeapi.cpp
	g++ -o moodgeist_pinger moodgeist_pinger.cpp skypeapi.cpp -Wall -Werror `curl-config --cflags` `curl-config --libs` -lX11
