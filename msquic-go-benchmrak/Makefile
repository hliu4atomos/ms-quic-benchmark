.PHONY: all build clean run-server run-client

all: build

build:
	CGO_LDFLAGS="-lnuma -ldl -lresolv -lpthread -static" go build -o bin/server ./server
	CGO_LDFLAGS="-lnuma -ldl -lresolv -lpthread -static" go build -o bin/client ./client

clean:
	rm -rf bin/server bin/client