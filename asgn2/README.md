Frank Isidore Gomez\
figomez - 1650550\
Assignment 2\
CSE 130

# Simple Web Logger

Makefile - The makefile for the Simple Web Logger. Compiles with "make", "make all", and "make split". Cleans with "clean".

httpserver.c - A simple web logger. Runs with ./httpserver \[-t threads\] \[-l logfile\] \<port\> On execution, it runs a HTTP server over localhost:\<port\>. There are 3 methods it supports, GET and PUT, which function identically to their HTTP/1 versions, and APPEND, which appends to the end of a file. The optional parameter threads currently has no functionality. The other alternate parameter, logfile, outputs a log of each request/response to the file pointed to by this parameter. On absence, it instead prints to stderr. Requests must be formatted as follows: METHOD URI HTTP\1.1\r\n(Headers followed by \r\n)\r\nMessage-Body. Files are stored locally on the host computer. Additionally, PUT and APPEND both require the Content-Length: # header, while GET does not. In addition, as the first part of a larger web server assignment, it also supports the Request-Id: # header, which has no functionality other than being logged as of now. 

README.md - The README for the Simple Web Logger. Contains basic knowledge on usage and design.


## Design + Error Handling
In a much better spot this time around, most of the time on this assignment was spent on fixing what wasn't working in assignment 1. Writing slightly under content length resulting in an infinite loop, writing exactly to content length not sending a response, removing timeouts because they didn't work before either, etc. As before, GET and APPEND mostly worked as intended, but as I finished PUT I realized APPEND suffered the same problems, and I was able to finally finish both of them. The logging itself was incredibly simple; pass all variables to an audit() function I wrote on the line right before each write(response).

## Basic Design:
Parse header. Heavy usage of strtok_r, but not many error cases this time.\
Once method is determined, switch statement\
-----GET:\
---------Verify file access/existence, then get the file and log the response. Simple.\
-----PUT:\
---------Verity valid filepath, create file if possible. Put up to content length, but after every write, make sure content length is still within reach. Audit after.\
-----APPEND:\
---------Verify file exists and permission, then append up to content length, still making sure content length is within reach (don't overflow). Audit after.\
-----Close, but on SIGINT/SIGTERM, fclose the logfile.

## Efficiency + Data Structure
My program reads 4095 bytes at a time to avoid a null pointer issue, but this time, it only writes exactly the number it needs to, instead of looping forever as it did in assignment 1. I removed a lot of redundant error checks as they aren't needed in this assignment. It can handle image/binary files, same as before. As for data structure, same as the last two times, I just used character arrays.


## Resources Used:
The open, read, and write man pages.\
CSE 130's Piazza and Discord.