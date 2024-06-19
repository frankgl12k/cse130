Frank Isidore Gomez\
figomez - 1650550\
Assignment 0\
CSE 130

# Simple File Splitter (I named it after a certain other class ;) )

Makefile - The makefile for the Simple File Splitter. Compiles with "make", "make all", and "make split". Cleans with "clean".

split.c - A simple file splitter. Runs with ./split \<single character delimiter\> \<file1\> \<file2\> .... (Up to theoretically infinite files.) On execution, prints the contents of each file to stdout, but replaces each instance of the delimiter with a newline. It does not modify the original file at all, and only prints the contents. It can also accept **stdin** as an argument by placing **-** as a file argument. When usings stdin, it only prints after receiving an EOF, which can be sent by pressing CTRL+D (it needs to be sent by itself to move onto the next file.)

README.md - The README for the Simple File Splitter. Contains basic knowledge on usage and design.


## Design + Error Handling
I kept it pretty simple, wrote all my code in main as it didn't seem too complicated that I would need to write functions. I used 6 defines to handle flags for multiple stdins, error returns, and the read-write loop. The main problems my code suffered as I was writing it was efficiency, as I started out by reading and writing 1 character at a time. I eventually changed this to reading 4096 bytes at a time, and writing however many bytes read returned, after replacing the delimiter in the buffer. The other main issue I had was errors to return, as I always returned 1 before I learned that I had to use errno. I used errno 22 for invalid number of arguments and multi-character delimiters, then let write and open set the remaining errnos. Other than that, this assignment was relatively simple, only had one valgrind error when I forgot to close files, but I fixed it quickly.

## Basic Design:
Verify correct arguments (>=3 and single character delimiter). Return error 22 if false.\
Check if file is file or stdin\
-----If stdin, read 4096, write number read until CTRL+D\
-----If file, open file\
--------If open success, read 4096, write number read, repeat until EOF\
--------If open failure, set error flag\
Once all files handled, check if error flag is set.\
-----If error flag is set, return with errno.\
Exit with SUCCESS!

## Efficiency + Data Structure
My program is sufficiently efficient as it reads 4096 bytes at a time, and only writes the number it has read. I also use write instead of printf for stdout, which means it can handle image/binary files. As for data structure and all that, I just use a character array buffer, as it's the most simple one I could think of + saw no reason to complicate it further.


## Resources Used:
The open, read, and write man pages.\
CSE 130's Piazza and Discord.