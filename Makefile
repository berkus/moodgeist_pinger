moodgeist_pinger: moodgeist_pinger.c
	g++ -o moodgeist_pinger moodgeist_pinger.c -Wall -Werror `curl-config --cflags` `curl-config --libs` -lX11
