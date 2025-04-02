CC = gcc
CFLAGS = -Wall -Werror -g -I/your-ms-quic-path/src/inc
LDFLAGS = -lmsquic -lnuma -L/your-msquic-path/build-static/bin/Release/

all: client server

client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

clean:
	rm -f client server

.PHONY: all clean
