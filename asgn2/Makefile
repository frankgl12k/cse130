all: httpserver

httpserver: httpserver.o
	clang -o httpserver httpserver.o -Wall -Werror -Wextra -pedantic

clean:
	rm -f httpserver.o httpserver a.out