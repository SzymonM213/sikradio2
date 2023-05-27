CC = g++
CFLAGS = -Wall -Wextra -pthread -O2 -std=gnu++17 -g
.PHONY: all clean sikradio-receiver sikradio-sender send receive

all: sikradio-receiver sikradio-sender

sikradio-sender sikradio-receiver: 
	$(CC) $(CFLAGS) $@.cpp err.h utils.h -o $@

send:
	sox -S "popek.mp3" -r 44100 -b 16 -e signed-integer -c 2 -t raw - | pv -q -L 176400 | ./sikradio-sender -a localhost -p 701

receive:
	./sikradio-receiver -a localhost -b 30000 | play -t raw -c 2 -r 44100 -b 16 -e signed-integer --buffer 32768 -

clean:
	rm -rf *.o sikradio-receiver sikradio-sender