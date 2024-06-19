Frank Isidore Gomez\
figomez - 1650550\
Assignment 4\
CSE 130

# Complex Multithreaded Server

Makefile - The makefile for the Complex Multithreaded Server. Compiles with "make", "make all", and "make split". Cleans with "clean". Running "make format" clang formats the file.

httpserver.c - A complex multithreaded server. Runs with ./httpserver \[-t threads\] \[-l logfile\] \<port\> On execution, it runs a HTTP server over localhost:\<port\>. There are 3 methods it supports, GET and PUT, which function identically to their HTTP/1 versions, and APPEND, which appends to the end of a file. The optional threads sets the number of threads that will be created. The other optional parameter, logfile, outputs a log of each request/response to the file pointed to by this parameter. On absence, it instead prints to stderr. Requests must be formatted as follows: METHOD URI HTTP\1.1\r\n(Headers followed by \r\n)\r\nMessage-Body. Files are stored locally on the host computer. Additionally, PUT and APPEND both require the Content-Length: # header, while GET does not. It also supports the Request-Id: # header, which displays in the logfile. It has no purpose other than for debugging.

README.md - The README for the Complex Multithreaded Server. Contains basic knowledge on usage and design.


## Design + Error Handling
The new parts to this assignment are the addition of search.h as a key-value library/table, and locking files based on readers vs writers (infinite readers but only ever one writer). For once, error handling was straightforwards, as I just used print statements to find where my code did and didn't enter. My biggest surprise was learning that including a single print statement after the first POLL changed my code to 100% accuracy, which made me realize my timeout was way too short. After fixing that, it was just a matter of remembering null characters at the end of the key in my key-value pairing, and I was done. I also combined PUT and APPEND into temporary file handlers, instead of having duplicates of the same code. This was crucial to avoiding data races/deadlocks, as each connection safely added to its own file rather than the main one directly.

## Basic Design:
Pre-Main: Create an arbitrary/defined number of semaphores to prevent data races.\
Main: Create -t number of pthreads by mallocing them.\
Create a dictionary using POSIX's search.h library. This will be used to manage locks on different files via a Global File Counter.\
Initialize all of the semaphores.\
Produce connfd's into an arbitrarily sized buffer.\
Threads:\
Consume connfd's via producer-consumer algorithm.\
Poll each connection. If no data, re-add to queue, try a different connection.\
Before each and every read, poll.\
Parse header. Heavy usage of strtok_r, but not many error cases this time.\
After finding the URI, lock threads and check if it exists in the dictionary. If it doesn't, add it, and store the current Global File Counter as the index in the semaphore array. If it does, retrieve the corresponding index for this file's semaphore.\
Once method is determined, if the method is PUT or APPEND, goto a tempFile section (Located after the main switch statement).\
Once method is determined for GET, and after PUT/APPEND have gone through their temporary sections, switch statement:\
-----GET:\
---------Verify file access/existence, then get the file and log the response. Simple.\
-----PUT:\
---------Verity valid filepath, create file if possible. Put up to Content-Length, which is guaranteed to be known from the tempFile section. Audit after.\
-----APPEND:\
---------Verify file exists and permission, then append the entire temporary file, as it is known that is the correct Content-Length. Audit after.\
After each operation, go back to the start of the thread and consume another connfd.\
-----tempFile (PUT and APPEND):\
---------Store the entire body's worth of Content-Length in a temporary file, then return to the Switch statement. The following PUT/APPEND algorithm from the prior assignments is implemented here:\
---------Verity valid filepath, create file if possible. Put up to content length, but after every write, make sure content length is still within reach.\
Close, but on SIGINT/SIGTERM, fclose the logfile, pthread_cancel the threads, and free the malloc'd pthread array.

## Efficiency + Data Structure (Unchanged from asgn3, kept as reference for the new parts)
My program reads 4095 bytes at a time to avoid a null pointer issue, same as before, and only writes exactly the number it needs. Each connection is contained in a struct called connObj, which stores the state of every variable at all times, so that a connection may be resumed at a later time. I named these after Java (Object) and Assembly (JumpRegister) methods, as they reminded me of their functionality. As for efficiency, I do not believe there is any busy waiting since I use poll(), although I could see some wasted time from polling connections that aren't ready, rather than using epoll.

## Efficiency + Data Structure (New for asgn4)
I had to implement some changes this time to fix the bugs in my asgn3 code. My main issue was the poll() function, as I only waited 1 second before moving to a new connection. Increasing this to 3 seconds changed my pass rate from "sometimes" to "deterministically 100% of the time." With that fixed, my only new section was combining my PUT and APPEND fragments into a tempFile handler, and locking access to files through the usage of a dictionary/key-value pair. Increasing my timeout didn't significantly increase my program's length, as the infinite loops/deadlocks from asgn3 lasted far longer than 3 seconds.

## Resources Used:
The open, read, and write man pages.\
My old CSE 156 code.\
CSE 130's Piazza and Discord.\
My friend James Quiaoit for telling me about the search.h library.