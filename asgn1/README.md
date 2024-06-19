Frank Isidore Gomez\
figomez - 1650550\
Assignment 1\
CSE 130

# Simple Web Server

Makefile - The makefile for the Simple Web Server. Compiles with "make", "make all", and "make split". Cleans with "clean".

httpserver.c - A simple web server. Runs with ./httpserver \<port\> On execution, it runs a HTTP server over localhost:\<port\>. There are 3 methods it supports, GET and PUT, which function identically to their HTTP/1 versions, and APPEND, which appends to the end of a file. Requests must be formatted as follows: METHOD URI HTTP\1.1\r\n(Headers followed by \r\n)\r\nMessage-Body. Files are stored locally on the host computer. Additionally, PUT and APPEND both require the Content-Length: # header, while GET does not. There is no functionality for headers other than Content-Length.

README.md - The README for the Simple Web Server. Contains basic knowledge on usage and design.


## Design + Error Handling
Going to be entirely honest here, this one took some out of me. There's a lot of ways to crash, and a lot of ways to do things incorrectly. My PUT isn't fully functional by the pipeline, but the output's good enough for me, so I'm sufficient with submitting it. My earliest bug was that none of my code ran at all, because I crashed on strtok_r repetitively. Otherwise, there was just a mess of trying things until they worked, rewriting them, reverting them, reusing my 156 code, unusing the 156 code since it was bad, etc. My biggest breakthrough was realizing that PUT might need a second read, which caused me to finally start passing PUT tests. Thankfully no valgrind errors.

## Basic Design:
Parse header. Lots of returns/responses if things go wrong, heavy usage of strtok_r.\
Once method is determined, switch statement\
-----GET:\
---------Verify file access/existence, then get the file. Simple.\
-----PUT:\
---------Verity valid filepath, create file if possible. Put up to content length.\
-----APPEND:\
---------Verify file exists and permission, then append up to content length.\
-----Close.

## Efficiency + Data Structure
My program only reads 2047 bytes at a time due to a null pointer issue, but it only writes the number it needs to. There's a lot of meticulous checks for this. It can handle image/binary files. As for data structure, same as last time, I just used character arrays.


## Resources Used:
The open, read, and write man pages.\
CSE 130's Piazza and Discord. \
My 156 code from last quarter. (It's all commented out now though.)