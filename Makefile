CC = g++
CFLAGS = -Wall -Wextra -O2 -std=gnu++17 -pthread
.PHONY: all clean sikradio-receiver sikradio-sender send receive

all: sikradio-receiver sikradio-sender

sikradio-sender sikradio-receiver remix: 
	$(CC) $(CFLAGS) $@.cpp err.h utils.h -o $@

popek:
	sox -S "popek.mp3" -r 44100 -b 16 -e signed-integer -c 2 -t raw - 2>/dev/null | pv -q -L 176400 | ./sikradio-sender -a 239.10.11.12 -P 2137 -n popek

foczki:
	sox -S "foczki.mp3" -r 44100 -b 16 -e signed-integer -c 2 -t raw - | pv -q -L 176400 | ./sikradio-sender -a 239.10.11.13 -n foczki

pudzian:
	sox -S "pudzian.mp3" -r 44100 -b 16 -e signed-integer -c 2 -t raw - | pv -q -L 176400 | ./sikradio-sender -a 239.10.11.14 -n pudzian

receive:
	./sikradio-receiver -b 30000 -n "popek" 2>chuj.txt | play -t raw -c 2 -r 44100 -b 16 -e signed-integer --buffer 32768 -

clean:
	rm -rf *.o sikradio-receiver sikradio-sender