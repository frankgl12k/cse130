#include <stdlib.h>
#include <stdio.h>
//#include <string.h>
#include <errno.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>

#define NO_ERRORS       0
#define ERORRS          1
#define KEEP_LOOPING    0
#define NO_MORE         1
#define ACCEPTING_INPUT 0
#define NO_MORE_STDIN   1

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Not enough arguments\n");
        fprintf(stdout, "usage: %s <split_char> [<file1> <file2> ...]\n", argv[0]);
        //warnx("%s: trying the block device", strerror(errno));
        //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
        return (22);
    }
    int errorFlag = NO_ERRORS;
    int stdinFlag = ACCEPTING_INPUT;
    //printf("Hello World %d\n", argc);
    //int length = 0;
    /*
    int justLookingForTheEnd = END_NOT_FOUND;
    while (justLookingForTheEnd == END_NOT_FOUND) {
        if (&argv[1][length] != "\0") {
            length++;
            //printf("Ding! %d\n", length);
        }
        else if (length > 2) {
            justLookingForTheEnd = END_FOUND;
            printf("Um\n");
            break;
        }
        else {
            justLookingForTheEnd = END_FOUND;
            printf("Um2\n");
            break;
        }
    }
    */
    //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
    if (argv[1][1] != 0) {
        //printf("%ld vs %ld\n", sizeof(&argv[1]), sizeof("a"));
        //printf("%s\n", &argv[1][1]);
        fprintf(stderr, "Cannot handle multi-character splits: %s\n", argv[1]);
        fprintf(stdout, "usage: %s <split_char> [<file1> <file2> ...]\n", argv[0]);
        return (22);
    }
    //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
    char buffer[4096] = { 0 };
    for (int i = 2; i < argc; i++) {
        if ((argv[i][0] == 45) && (argv[i][1] == 0) && (stdinFlag == ACCEPTING_INPUT)) {
            //fflush(stdout);
            stdinFlag = NO_MORE_STDIN;
            //buffer[0] = 0;
            //buffer[1] = 0;
            int n = read(STDIN_FILENO, buffer, 4096);
            //int loopFlag = KEEP_LOOPING;
            int loopFlag = KEEP_LOOPING;
            int j = 0;
            int kai = 0;
            while (loopFlag == KEEP_LOOPING) {
                for (int m = 0; m < n; m++) {
                    if (buffer[m] == argv[1][0]) {
                        buffer[m] = '\n';
                    }
                }
                kai = write(STDOUT_FILENO, buffer, n);
                if (kai == -1) {
                    fprintf(stderr, "%s: No space left on device\n", argv[0]);
                    errorFlag = ERORRS;
                    break;
                }
                n = read(STDIN_FILENO, buffer, 4096);
                if (n == 0) {
                    loopFlag = NO_MORE;
                }
            }

            //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
            /**
                j++;
                if (j == 1024) {
                    
                    j = 0;
                    if (n == 0) {
                        loopFlag = NO_MORE;
                        //break;
                    }
                }**/
        } else {
            int currentFile = open(argv[i], O_RDONLY);
            if (currentFile != -1) {
                int n = read(currentFile, buffer, 4096);

                int loopFlag = KEEP_LOOPING;
                int j = 0;
                int kai = 0;
                while (loopFlag == KEEP_LOOPING) {
                    for (int m = 0; m < n; m++) {
                        if (buffer[m] == argv[1][0]) {
                            buffer[m] = '\n';
                        }
                    }
                    kai = write(STDOUT_FILENO, buffer, n);
                    if (kai == -1) {
                        fprintf(stderr, "%s: No space left on device\n", argv[0]);
                        errorFlag = ERORRS;
                        break;
                    }
                    /**
                    if (buffer[j] == argv[1][0]) {
                        kai = write(STDOUT_FILENO, "\n", 1);
                        if (kai == -1) {
                            fprintf(stderr, "%s: No space left on device\n", argv[0]);
                            errorFlag = ERORRS;
                            break;
                        }
                        buffer[j] = 0;
                    } else if (buffer[j] == 0) {
                        loopFlag = NO_MORE;
                    } **/
                    //else {
                    //    kai = write(STDOUT_FILENO, (void *) &buffer[j], 1);
                    //    if (kai == -1) {
                    //        fprintf(stderr, "%s: No space left on device\n", argv[0]);
                    //        errorFlag = ERORRS;
                    //        break;
                    //    }
                    //    buffer[j] = 0;
                    //}
                    //j++;
                    //if (j == 4096) {
                    n = read(currentFile, buffer, 4096);
                    //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
                    //    j = 0;
                    if (n == 0) {
                        loopFlag = NO_MORE;
                        //fflush(stdout);
                        //break;
                    }
                    //}
                }
                close(currentFile);

            } else {
                //fflush(stdout);
                //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
                fprintf(stderr, "%s: %s: No such file or directory\n", argv[0], argv[i]);
                //warnx("%s: %s: trying the block device", argv[i], strerror(errno));
                errorFlag = ERORRS;
            }
        }
    }
    if (errorFlag == ERORRS) {
        //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
        return (errno);
    }
    //fprintf( stderr, "Error is %s (errno=%d)\n", strerror( errno ), errno );
    return (0);
}