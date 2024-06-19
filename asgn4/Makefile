all: httpserver

httpserver: httpserver.o
	clang -pthread -o httpserver httpserver.o -Wall -Werror -Wextra -pedantic

format:
	clang-format -i -style=file *.[ch]

clean:
	rm -f httpserver.o httpserver a.out