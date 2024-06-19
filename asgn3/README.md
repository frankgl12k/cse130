Frank Isidore Gomez\
figomez - 1650550\
Assignment 3\
CSE 130

# Simple Multithreaded Server

Makefile - The makefile for the Simple Multithreaded Server. Compiles with "make", "make all", and "make split". Cleans with "clean". "make format" clang formats the file.

httpserver.c - A simple multithreaded server. Runs with ./httpserver \[-t threads\] \[-l logfile\] \<port\> On execution, it runs a HTTP server over localhost:\<port\>. There are 3 methods it supports, GET and PUT, which function identically to their HTTP/1 versions, and APPEND, which appends to the end of a file. The optional threads sets the number of threads that will be created. The other optional parameter, logfile, outputs a log of each request/response to the file pointed to by this parameter. On absence, it instead prints to stderr. Requests must be formatted as follows: METHOD URI HTTP\1.1\r\n(Headers followed by \r\n)\r\nMessage-Body. Files are stored locally on the host computer. Additionally, PUT and APPEND both require the Content-Length: # header, while GET does not. In addition, as the first part of a larger web server assignment, it also supports the Request-Id: # header, which has no functionality other than being logged as of now. 

README.md - The README for the Simple Multithreaded Server. Contains basic knowledge on usage and design.


## Design + Error Handling
This assignment proved to be much more difficult than asgn2 by a long shot. My greatest achievement here was designing a coherent "Object" that stores the current state of every variable for each connection. On poll() timeout, it stores these variables, then returns to the start of the threads, "jumping" back later if the connection is ready again. The two core functions are POLL(), which calls poll and resets if there's nothing available, and RESTORE(), which returns to the last jumping off point. (Essentially, jal from assembly.) Error wise, I believe I handled all the seg-faults, and finally handled the case of receiving Request-Id and Content-Length in reverse order, as they were causing a segfault earlier on.

## Basic Design:
Main: Create -t number of pthreads by mallocing them.\
Produce connfd's into an arbitrarily sized buffer.\
Threads:\
Consume connfd's via producer-consumer algorithm.\
Poll each connection. If no data, re-add to queue, try a different connection.\
Before each and every read, poll.\
Parse header. Heavy usage of strtok_r, but not many error cases this time.\
Once method is determined, switch statement\
-----GET:\
---------Verify file access/existence, then get the file and log the response. Simple.\
-----PUT:\
---------Verity valid filepath, create file if possible. Put up to content length, but after every write, make sure content length is still within reach. Audit after.\
-----APPEND:\
---------Verify file exists and permission, then append up to content length, still making sure content length is within reach (don't overflow). Audit after.\
After each operation, go back to the start of the thread and consume another connfd.\
-----Close, but on SIGINT/SIGTERM, fclose the logfile, pthread_cancel the threads, and free the malloc'd pthread array.

## Efficiency + Data Structure
My program reads 4095 bytes at a time to avoid a null pointer issue, same as before, and only writes exactly the number it needs. Each connection is contained in a struct called connObj, which stores the state of every variable at all times, so that a connection may be resumed at a later time. I named these after Java (Object) and Assembly (JumpRegister) methods, as they reminded me of their functionality. As for efficiency, I do not believe there is any busy waiting since I use poll(), although I could see some wasted time from polling connections that aren't ready, rather than using epoll.


## Resources Used:
The open, read, and write man pages.\
My old CSE 156 code.\
CSE 130's Piazza and Discord.