#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

/**
   Converts a string to an 16 bits unsigned integer.
   Returns 0 if the string is malformed or out of the range.
 */
uint16_t strtouint16(char number[]) {
    char *last;
    long num = strtol(number, &last, 10);
    if (num <= 0 || num > UINT16_MAX || *last != '\0') {
        return 0;
    }
    return num;
}
uint32_t strtouint32(char number[]) {
    char *last;
    long num = strtol(number, &last, 10);
    if (num <= 0 || num > UINT32_MAX || *last != '\0') {
        return 0;
    }
    return num;
}

/**
   Creates a socket for listening for connections.
   Closes the program and prints an error message on error.
 */
int create_listen_socket(uint16_t port) {
    struct sockaddr_in addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    if (listenfd < 0) {
        err(EXIT_FAILURE, "socket error");
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr *) &addr, sizeof addr) < 0) {
        err(EXIT_FAILURE, "bind error");
    }

    if (listen(listenfd, 500) < 0) {
        err(EXIT_FAILURE, "listen error");
    }

    return listenfd;
}
#define BAD_REQUEST           0
#define GET_REQUEST           1
#define PUT_REQUEST           2
#define APPEND_REQUEST        3
#define UNIMPLEMENTED_REQUEST 4
#define OTHER_FAILURE         5

#define URI_LOST  0
#define URI_FOUND 1

#define NO_VERSION    0
#define VERSION_FOUND 1

#define NO_NEW_FILE  0
#define FILE_CREATED 1

#define METHOD_SEEKING   0
#define URI_SEEKING      1
#define VERSION_SEEKING  2
#define LENGTH_SEEKING   3
#define BODY_SEEKING     4
#define REQUEST_COMPLETE 5

#define NOT_EXPECTED      0
#define CONTINUE_EXPECTED 1

#define BUFFER_LENGTH 2048

void failRequestHandler(int connfd, int requestType) {
    int m = 0;
    char requestError[BUFFER_LENGTH] = { 0 };
    //fflush(stdout);
    switch (requestType) {
    case UNIMPLEMENTED_REQUEST:
        snprintf(requestError, BUFFER_LENGTH,
            "HTTP/1.1 501 Not Implemented\r\nContent-Length: %ld\r\n\r\nNot Implemented\n",
            strlen("Not Implemented\n"));
        m = write(connfd, requestError, strlen(requestError));
        break;
    case BAD_REQUEST:
        snprintf(requestError, BUFFER_LENGTH,
            "HTTP/1.1 400 Bad Request\r\nContent-Length: %ld\r\n\r\nBad Request\n",
            strlen("Bad Request\n"));
        m = write(connfd, requestError, strlen(requestError));
        break;
    default:
        snprintf(requestError, BUFFER_LENGTH,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: %ld\r\n\r\nInternal Server "
            "Error\n",
            strlen("Internal Server Error\n"));
        m = write(connfd, requestError, strlen(requestError));
        break;
    }
    memset(requestError, 0, BUFFER_LENGTH);
    close(connfd);
    return;
}

void failOpenHandler(int connfd, int requestType, int openStatus) {
    int j = 0;
    char openError[BUFFER_LENGTH] = { 0 };
    switch (openStatus) {
    case EACCES:
        snprintf(openError, BUFFER_LENGTH,
            "HTTP/1.1 403 Forbidden\r\nContent-Length: %ld\r\n\r\nForbidden\n",
            strlen("Forbidden\n"));
        j = write(connfd, openError, strlen(openError));
        break;
    case ENOENT:
        snprintf(openError, BUFFER_LENGTH,
            "HTTP/1.1 404 Not Found\r\nContent-Length: %ld\r\n\r\nNot Found\n",
            strlen("Not Found\n"));
        j = write(connfd, openError, strlen(openError));
        break;
    default:
        snprintf(openError, BUFFER_LENGTH,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: %ld\r\n\r\nInternal Server "
            "Error\n",
            strlen("Internal Server Error\n"));
        j = write(connfd, openError, strlen(openError));
        break;
    }
    memset(openError, 0, BUFFER_LENGTH);
    close(connfd);
    return;
}

#define checkReadWrite(valueRW)                                                                    \
    if (valueRW < 0) {                                                                             \
        failRequestHandler(connfd, BAD_REQUEST);                                                   \
        memset(requestBuffer, 0, BUFFER_LENGTH);                                                   \
        return;                                                                                    \
    }

#define TIMEOUT_CHECK(valueRW)                                                                     \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            failRequestHandler(connfd, OTHER_FAILURE);                                             \
            return;                                                                                \
        }                                                                                          \
    }

#define TIMEOUT_CHECK_LOOP(valueRW)                                                                \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            break;                                                                                 \
        } else {                                                                                   \
            failRequestHandler(connfd, BAD_REQUEST);                                               \
            memset(requestBuffer, 0, BUFFER_LENGTH);                                               \
            return;                                                                                \
        }                                                                                          \
    }
#define checkReadWriteAppend(valueRW)                                                              \
    if (valueRW < 0) {                                                                             \
        failRequestHandler(connfd, BAD_REQUEST);                                                   \
        memset(requestBuffer, 0, BUFFER_LENGTH);                                                   \
        return;                                                                                    \
    }

void handle_connection(int connfd) {
    // make the compiler not complain
    //while (1) {
    int currentState = METHOD_SEEKING;
    int requestType = BAD_REQUEST;
    int uriStatus = URI_LOST;
    int versionStatus = NO_VERSION;
    int newFileStatus = NO_NEW_FILE;
    uint32_t contentLength = 0;

    char requestBuffer[BUFFER_LENGTH] = { 0 };
    //int requestIntBuffer[BUFFER_LENGTH] = {0};
    char requestCopy[BUFFER_LENGTH] = { 0 };
    char finalBuffer[BUFFER_LENGTH] = { 0 };
    //char putBuffer[BUFFER_LENGTH] = {0};
    char goodResponse[BUFFER_LENGTH] = { 0 };
    char requestTypeString[16] = { 0 };
    char uriString[20] = { 0 };
    char betterURIString[20] = { 0 };
    char endofHeader[] = "\r\n\r\n";
    char *substring;
    int m = 0;
    char *trueContentPointer;
    char trueContentLength[10] = { 0 };
    char *trueContentToken;
    int expectedContinue = NOT_EXPECTED;

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    //setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    //printf("%ld\n",strlen("PUT /foo.txt HTTP/1.1\r\ndsfgdfgz: LMAO\r\nContent-Length: 20\r\n\r\n"));

    //int stopblocking = fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL, 0) | O_NONBLOCK);

    //if (stopblocking == -1) {
    //    failRequestHandler(connfd, OTHER_FAILURE);
    //    return;
    //}

    int n = read(connfd, requestBuffer, BUFFER_LENGTH - 1);
    //for (int i = 0; i < n; i++) {
    //    printf("%c", requestBuffer[i]);
    //}
    //fflush(stdout);
    //memset( requestIntBuffer, 0, BUFFER_LENGTH);

    //return;
    //TIMEOUT_CHECK(n);
    //printf("%s\n", requestBuffer);
    if (n < 0) {
        failRequestHandler(connfd, OTHER_FAILURE);
        return;
    }
    //printf("%s", requestBuffer);
    char *tempBody;
    char *body = requestBuffer;

    strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
    char *remainder = requestCopy;
    substring = strtok_r(remainder, " ", &remainder);
    if (substring == NULL) {
        failRequestHandler(connfd, OTHER_FAILURE);
        return;
    }
    while (currentState != REQUEST_COMPLETE) {
        switch (currentState) {
        case METHOD_SEEKING:
            //printf("%ld\n",strlen(substring));
            if (strstr(substring, "GET") != NULL) {
                if (strlen(substring) == 3) {
                    requestType = GET_REQUEST;
                } else {
                    requestType = UNIMPLEMENTED_REQUEST;
                }
                //printf("Append request!\n");
            } else if (strstr(substring, "PUT") != NULL) {
                if (strlen(substring) == 3) {
                    requestType = PUT_REQUEST;
                } else {
                    requestType = UNIMPLEMENTED_REQUEST;
                }
                //requestType = PUT_REQUEST;
                //printf("Put request!\n");
            } else if (strstr(substring, "APPEND") != NULL) {
                if (strlen(substring) == 6) {
                    requestType = APPEND_REQUEST;
                } else {
                    requestType = UNIMPLEMENTED_REQUEST;
                }
                //requestType = APPEND_REQUEST;
                //printf("Get request!\n");
            }
            /*
                else if (strstr(substring, "HEAD") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "POST") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "DELETE") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "CONNECT") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "OPTIONS") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "TRACE") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }
                else if (strstr(substring, "PATCH") != NULL) {
                    requestType = UNIMPLEMENTED_REQUEST;
                    goto failRequest;
                }*/
            else {
                requestType = UNIMPLEMENTED_REQUEST;
                //printf("Unimplemented!\n");
            }
            if (requestType == UNIMPLEMENTED_REQUEST) {
                strncpy(requestTypeString, substring, sizeof(requestTypeString));
                if (strlen(requestTypeString) > 8 || strlen(requestTypeString) == 0) {
                    requestType = BAD_REQUEST;
                    failRequestHandler(connfd, requestType);
                    memset(requestBuffer, 0, BUFFER_LENGTH);
                    return;
                    //printf("Bad unimplemented!\n");
                }
                failRequestHandler(connfd, requestType);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
                //goto failRequest;
            }
            //substring = strtok_r(NULL, "/", &remainder);
            substring = strtok_r(NULL, " ", &remainder);
            if (substring == NULL) {
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }
            currentState = URI_SEEKING;
            break;
        case URI_SEEKING:
            if (strlen(substring) <= 19 && strlen(substring) > 0) {
                uriStatus = URI_FOUND;
                strncpy(uriString, substring, sizeof(uriString));
                if (uriString[0] == '/') {
                    //strncpy(betterURIString, &uriString[1], sizeof(betterURIString));
                    strncpy(betterURIString, uriString, sizeof(betterURIString));

                    //printf("better: %s\n", betterURIString);
                } else {
                    strncpy(betterURIString, uriString, sizeof(betterURIString));
                    failRequestHandler(connfd, BAD_REQUEST);
                    memset(requestBuffer, 0, BUFFER_LENGTH);
                    return;
                    //printf("basic: %s\n", betterURIString);
                }

                //printf("%s and %s\n", uriString, betterURIString);
                //for (int k = 0; k < strlen(uriString)-1; k++) {
                //    uriString[k] = uriString[k+1];
                //}
                //uriString[strlen(uriString)-1] = '\0';
            } else {
                //printf("No uri!\n");
                uriStatus = URI_LOST;
                requestType = BAD_REQUEST;
                failRequestHandler(connfd, requestType);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
                //goto failRequest;
            }
            substring = strtok_r(NULL, "\r\n", &remainder);
            if (substring == NULL) {
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }
            currentState = VERSION_SEEKING;
            break;
        case VERSION_SEEKING:
            if (strcmp(substring, "HTTP/1.1") == 0) {
                versionStatus = VERSION_FOUND;
            } else {
                //printf("Bad version!\n");
                versionStatus = NO_VERSION;
                requestType = BAD_REQUEST;
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
                //goto failRequest;
            }
            while (substring != NULL) {
                substring = strtok_r(NULL, "END", &remainder);
            }
            /*
            substring = strtok_r(NULL, ": ", &remainder);
            if (substring == NULL) {
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }
            */
            if (requestType != GET_REQUEST && requestType != BAD_REQUEST
                && requestType != UNIMPLEMENTED_REQUEST) {
                currentState = LENGTH_SEEKING;
            } else {
                currentState = REQUEST_COMPLETE;
            }

            break;
        case LENGTH_SEEKING:
            /*
            substring = strtok_r(NULL, "\r\n", &remainder);
            if (substring == NULL) {
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }*/
            memset(requestCopy, 0, BUFFER_LENGTH);
            strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
            trueContentPointer = strstr(requestCopy, "\r\nContent-Length: ");
            //printf(trueContentPointer);
            if (trueContentPointer == NULL && requestType != GET_REQUEST) {
                failRequestHandler(connfd, BAD_REQUEST);
                return;
            }

            trueContentToken = trueContentPointer;
            trueContentPointer = strtok_r(trueContentToken, " ", &trueContentToken);
            trueContentPointer = strtok_r(NULL, "\r\n", &trueContentToken);
            //printf(trueContentPointer);
            if (trueContentToken != NULL) {
                strncpy(trueContentLength, trueContentPointer, 10);
            } else {
                failRequestHandler(connfd, BAD_REQUEST);
                return;
            }
            //printf("%s\n",trueContentLength);
            contentLength = strtouint32(trueContentLength);
            //printf("%d\n",contentLength);
            //printf("%d\n",atoi(trueContentLength));

            while (trueContentPointer != NULL) {
                trueContentPointer = strtok_r(NULL, "END", &trueContentToken);
            }
            //printf("%d and %s and %c\n", contentLength, trueContentLength, trueContentLength[0]);
            if (trueContentLength[0] == '-') {
                //printf("Um\n");
                requestType = BAD_REQUEST;
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }
            if ((contentLength == 0) && (strstr(requestBuffer, "Content-Length: 0") == NULL)) {
                requestType = BAD_REQUEST;
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }

            if (strstr(requestBuffer, "\r\n\r\n") == NULL) {
                requestType = BAD_REQUEST;
                failRequestHandler(connfd, BAD_REQUEST);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }

            if (contentLength <= 0 && requestType != GET_REQUEST) {
                if (strstr(requestBuffer, "\r\nContent-Length: ") != NULL) {
                    //Nothing
                } else {
                    requestType = BAD_REQUEST;
                    failRequestHandler(connfd, BAD_REQUEST);
                    memset(requestBuffer, 0, BUFFER_LENGTH);
                    return;
                    //printf("Bad length!\n");
                    //goto failRequest;
                }
            }
            currentState = REQUEST_COMPLETE;
            break;
        default: break;
        }
    }
    if (strstr(requestBuffer, "Expect: 100-continue") != NULL) {
        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 100 Continue\r\n");
        expectedContinue = CONTINUE_EXPECTED;
    }
    //Check if contentLength == 0

    //failRequest:

    //memset(substring, 0, BUFFER_LENGTH);
    if (requestType == UNIMPLEMENTED_REQUEST || requestType == BAD_REQUEST) {
        //printf("Danger request!\n");
        failRequestHandler(connfd, requestType);
        memset(requestBuffer, 0, BUFFER_LENGTH);
        return;
    }
    int activeFile;
    int openStatus;
    int rwStatus = 0;
    int size = 0;
    struct stat st;
    if (stat(&betterURIString[1], &st) == -1) {
        //printf("hi\n");
        //failOpenHandler(connfd, requestType, ENOENT);
        //memset(requestBuffer, 0, BUFFER_LENGTH);
        //return;
    } else {
        size = st.st_size;
    }
    switch (requestType) {
    case GET_REQUEST:
        memset(goodResponse, 0, BUFFER_LENGTH);

        if (access(&betterURIString[1], R_OK) != 0) {
            failOpenHandler(connfd, EACCES, EACCES);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failOpenHandler(connfd, ENOENT, ENOENT);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (strstr(&betterURIString[1], "/") != NULL) {
            failRequestHandler(connfd, BAD_REQUEST);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_RDONLY);

        openStatus = errno;
        if (activeFile == -1) {
            //printf("%s and %d\n", uriString, openStatus);
            failRequestHandler(connfd, openStatus);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }

        //stat(betterURIString, &st);

        //printf("Hi\n");
        snprintf(
            goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", size);
        //printf(goodResponse);
        write(connfd, goodResponse, strlen(goodResponse));
        //memset(finalBuffer, 0 , BUFFER_LENGTH);
        int p = read(activeFile, finalBuffer, BUFFER_LENGTH);
        checkReadWrite(p);
        //printf("%s\n", finalBuffer);
        while (p != 0) {
            rwStatus = write(connfd, finalBuffer, p);
            checkReadWrite(rwStatus);
            //printf("%d and %s\n", t, finalBuffer);
            memset(finalBuffer, 0, p);

            p = read(activeFile, finalBuffer, BUFFER_LENGTH);
            checkReadWrite(p);
        }
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        //printf
        break;
    case PUT_REQUEST:
        //printf("Got in here.\n");
        if (strstr(&betterURIString[1], "/") != NULL) {
            failRequestHandler(connfd, BAD_REQUEST);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            if (errno != ENOENT) {
                failOpenHandler(connfd, PUT_REQUEST, errno);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            } else {
                newFileStatus = FILE_CREATED;
            }

            //printf("no\n");
            //failOpenHandler(connfd, requestType, ENOENT);
            //memset(requestBuffer, 0, BUFFER_LENGTH);
            //return;
        }
        if ((access(&betterURIString[1], W_OK) != 0) && (newFileStatus != FILE_CREATED)) {
            failOpenHandler(connfd, PUT_REQUEST, EACCES);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        openStatus = errno;
        //printf("Hi!2\n");

        /*if (activeFile == -1) {
            //printf("hi!\n");
            
            int folderNumber = 0;
            int folderSplit[19] = { 0 };
            char path[19] = { 0 };
            int tempFolderHolder[19] = { 0 };
            int folderLength = 0;
            int fileWidth = 0;
            char singlePathFolder[19] = { 0 };
            int numberOfFolders = 0;
            for (int folderSetup = 0; folderSetup <= strlen(betterURIString); folderSetup++) {

                if (betterURIString[folderSetup] == '/') {
                    tempFolderHolder[fileWidth] = folderLength;
                    fileWidth++;
                    folderLength = -1;
                    numberOfFolders++;
                    //printf("Found slash!\n");
                } else if (betterURIString[folderSetup] == '\0') {
                    tempFolderHolder[fileWidth] = folderLength;
                    fileWidth++;
                    //printf("Found end!\n");
                    break;
                }
                folderLength++;
            }
            folderNumber = fileWidth;
            for (int folderarraySetup = 0; folderarraySetup <= fileWidth; folderarraySetup++) {
                folderSplit[folderarraySetup] = tempFolderHolder[folderarraySetup];
                //printf("Ping! %d\n", folderSplit[folderarraySetup]);
            }
            strcpy(path, betterURIString);
            int totalPath = 0;
            int layersDeep = 0;
            for (int folderChecker = 0; folderChecker < folderNumber; folderChecker++) {
                for (int singleFolder = 0; singleFolder < folderSplit[folderChecker];
                     singleFolder++) {
                    //printf("%d\n", singleFolder);
                    singlePathFolder[singleFolder] = path[totalPath];
                    totalPath++;
                }
                totalPath++;
                //printf("F+%s\n", singlePathFolder);
                if (strlen(singlePathFolder) != 0) {
                    //printf("Danger!\n");

                    if (folderChecker < numberOfFolders) {
                        if (chdir(singlePathFolder) == -1) {
                            //printf("Deep!\n");
                            if (mkdir(singlePathFolder, 0777) == 0) {
                                //printf("Deeper!\n");
                                chdir(singlePathFolder);
                                layersDeep++;
                            } else {
                                //printf("Foil\n");
                            }
                        } else {
                            //printf("Deep2!\n");
                            layersDeep++;
                        }
                    }
                }

                //printf("%d\n", nextSn[currentClient]);
                
            
            char *semifinalURIString;
            char finalURIString[20] = { 0 };
            char *finalRemainder = betterURIString;
            int t = 0;
            semifinalURIString = strtok_r(finalRemainder, "/", &finalRemainder);
            strncpy(finalURIString, semifinalURIString, strlen(finalURIString));
            //printf("In here? %s\n", semifinalURIString);
            while (semifinalURIString != NULL) {
                
                printf("In here? %s\n", semifinalURIString);
                if (semifinalURIString != NULL) {
                    if (mkdir(semifinalURIString, 0777) != 0) {
                        if(chdir(semifinalURIString) != 0) {
                            printf("wa happun\n");
                        }
                        else {
                            printf("Changing directory!\n");
                            t++;
                        }
                        
                        
                    }
                }
                semifinalURIString = strtok_r(NULL, "/", &finalRemainder);
                
            }
            while (t > 0) {
                chdir("..");
                printf("Changing up!\n");
                t--;
            }
            //if (stat(finalURIString, &st) == -1) {
            //    printf("We in here? %s\n", finalURIString);
            //    mkdir(finalURIString, 0700);
            //}
            //printf("%d\n", mkdir(betterURIString, 0700));
            

            //printf("Other way out!\n");
            //}
            if (stat(betterURIString, &st) == -1) {
                //printf("no\n");
                //failOpenHandler(connfd, requestType, ENOENT);
                //memset(requestBuffer, 0, BUFFER_LENGTH);
                //return;
            }
            activeFile = open(
                &betterURIString[1], O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
            openStatus = errno;
            //printf("activeFile is %d\n", activeFile);
            //printf("Errno is %d\n", errno);
            //for (int i = 0; i < sizeof(singlePathFolder); i++) {
            //    //printf("Hi\n");
            //    singlePathFolder[i] = '\0';
            //    //printf("Bye!\n");
            //}
            //printf("did we get here\n");
            //for (int j = 0; j < layersDeep; j++) {
            //    chdir("..");
            //printf("Rising!\n");
            //}
            //strerror(errno);
            newFileStatus = FILE_CREATED;
        }*/
        //printf("Hi!1\n");
        if (activeFile == -1) {
            failOpenHandler(connfd, requestType, openStatus);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        //printf("Hi!\n");

        if (expectedContinue == CONTINUE_EXPECTED) {
            //write(connfd, goodResponse, strlen(goodResponse));
            memset(goodResponse, 0, BUFFER_LENGTH);
            n = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            if (n < contentLength) {
                write(activeFile, finalBuffer, n);
            } else {
                write(activeFile, finalBuffer, contentLength);
            }

            goto expectedContinueStart;
        } else {
            memset(goodResponse, 0, BUFFER_LENGTH);
        }
        tempBody = strstr(requestBuffer, endofHeader);
        char *cursedPointer = tempBody + strlen("\r\n\r\n");
        //printf("Request %p vs Pointer %p vs Int %ld\n", &requestBuffer, cursedPointer, (cursedPointer - &requestBuffer[0]));
        int rollingContent = contentLength;
        int trueHeaderLength = cursedPointer - &requestBuffer[0];
        if (tempBody) {
            if (n - trueHeaderLength > contentLength) {
                //printf("We writing? %s\n", cursedPointer);
                write(activeFile, cursedPointer, contentLength);
                //goto putComplete;
                write(connfd, goodResponse, strlen(goodResponse));
                memset(finalBuffer, 0, BUFFER_LENGTH);
                memset(goodResponse, 0, BUFFER_LENGTH);
                return;
            } else {
                //printf("Header: %s", requestBuffer);
                rollingContent
                    = contentLength - write(activeFile, cursedPointer, n - trueHeaderLength);
            }
        }
        //printf("%ld\n", sizeof(cursedPointer));
        //int rollingContent = contentLength;
        /*
        if (tempBody) {
            //printf(cursedPointer);
            if (sizeof(cursedPointer) < contentLength) {
                printf("In here\n");
                rollingContent
                    = contentLength - write(activeFile, cursedPointer, sizeof(&cursedPointer[0]));

            } else {
                write(activeFile, cursedPointer, contentLength);
                //goto putComplete;
                write(connfd, goodResponse, strlen(goodResponse));
                memset(finalBuffer, 0, BUFFER_LENGTH);
                memset(goodResponse, 0, BUFFER_LENGTH);
                break;
            }
            //printf(cursedPointer);
        } else {
            //printf("nonya\n");
        }*/
        tempBody = NULL;
        cursedPointer = NULL;
        //while (tempBody != NULL) {
        //    tempBody = strtok_r(NULL, "none", &body);

        //    //write(activeFile, tempBody, strlen(tempBody));
        //}
        //printf("Did we escape? %d\n", n);
        //memset(tempBody, 0, BUFFER_LENGTH);
    expectedContinueStart:
        memset(requestBuffer, 0, BUFFER_LENGTH);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int q;
        if (n == BUFFER_LENGTH - 1) {
            //printf("Are we in here?\n");
            q = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWrite(q);
            while (q > 0) {
                if (q > rollingContent) {
                    rwStatus = write(activeFile, finalBuffer, rollingContent);
                    checkReadWrite(rwStatus);
                    //goto putComplete;
                    break;
                }
                rwStatus = write(activeFile, finalBuffer, q);
                checkReadWrite(rwStatus);
                rollingContent = rollingContent - rwStatus;

                memset(finalBuffer, 0, BUFFER_LENGTH);

                q = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
                TIMEOUT_CHECK_LOOP(q);
                //checkReadWrite(q);
            }
        }
        //putComplete:
        //printf("Found a way back home.\n");
        memset(goodResponse, 0, BUFFER_LENGTH);
        if (newFileStatus == FILE_CREATED) {
            snprintf(goodResponse, BUFFER_LENGTH,
                "HTTP/1.1 201 Created\r\nContent-Length: %ld\r\n\r\nCreated\n",
                strlen("Created\n"));
        } else {
            snprintf(goodResponse, BUFFER_LENGTH,
                "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n", strlen("OK\n"));
        }
        //printf("True Response: \n%sTrue Size: %ld\n", goodResponse, strlen(goodResponse));
        write(connfd, goodResponse, strlen(goodResponse));
        //write(connfd, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n", strlen("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n"));
        //memset(finalBuffer, 0, BUFFER_LENGTH);
        //int L = read(connfd, finalBuffer, BUFFER_LENGTH);
        //printf("%s\n",finalBuffer);
        //write(connfd, "OK\n", strlen("OK\n"));
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        //close(activeFile);
        //exit(0);
        break;
    case APPEND_REQUEST:
        if (strstr(&betterURIString[1], "/") != NULL) {
            failRequestHandler(connfd, BAD_REQUEST);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failOpenHandler(connfd, requestType, errno);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_RDWR | O_APPEND);
        openStatus = errno;
        if (activeFile == -1) {
            failOpenHandler(connfd, requestType, openStatus);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        /*
        if (expectedContinue == CONTINUE_EXPECTED) {
            write(connfd, goodResponse, strlen(goodResponse));
            memset(goodResponse, 0, BUFFER_LENGTH);
        }
        else {
            memset(goodResponse, 0, BUFFER_LENGTH);
        }*/
        //printf("Made it past.\n");
        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n",
            strlen("OK\n"));

        tempBody = strstr(requestBuffer, endofHeader);
        char *cursedPointer2 = tempBody + strlen("\r\n\r\n");
        int rollingContent2 = contentLength;
        int trueHeaderLength2 = cursedPointer2 - &requestBuffer[0];
        if (tempBody) {
            if (n - trueHeaderLength2 > contentLength) {
                write(activeFile, cursedPointer2, contentLength);
                write(connfd, goodResponse, strlen(goodResponse));
                memset(finalBuffer, 0, BUFFER_LENGTH);
                memset(goodResponse, 0, BUFFER_LENGTH);
                return;
            } else {
                rollingContent2
                    = contentLength - write(activeFile, cursedPointer2, n - trueHeaderLength2);
            }
        }
        /*if (strlen(cursedPointer2) < contentLength) {
                rollingContent2
                    = contentLength - write(activeFile, cursedPointer2, strlen(cursedPointer2));
            } else {
                write(activeFile, cursedPointer2, contentLength);
                write(connfd, goodResponse, strlen(goodResponse));
                memset(finalBuffer, 0, BUFFER_LENGTH);
                memset(goodResponse, 0, BUFFER_LENGTH);
                break;
            }*/
        //printf(cursedPointer);
        tempBody = NULL;
        cursedPointer2 = NULL;

        //printf("Made it present.\n");
        //while (tempBody != NULL) {
        //    tempBody = strtok_r(NULL, "none", &body);

        //    //write(activeFile, tempBody, strlen(tempBody));
        //}
        //printf("Did we escape?\n");
        //memset(tempBody, 0, BUFFER_LENGTH);
        memset(requestBuffer, 0, BUFFER_LENGTH);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int s;
        if (n == BUFFER_LENGTH - 1) {
            s = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWriteAppend(s);
            while (s > 0) {
                if (s > rollingContent2) {
                    rwStatus = write(activeFile, finalBuffer, rollingContent2);
                    checkReadWriteAppend(rwStatus);
                    //goto appendComplete;
                    break;
                }
                rwStatus = write(activeFile, finalBuffer, s);
                checkReadWriteAppend(rwStatus);
                rollingContent2 = rollingContent2 - rwStatus;

                memset(finalBuffer, 0, BUFFER_LENGTH);

                s = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
                TIMEOUT_CHECK_LOOP(s);
                //checkReadWrite(s);
            }
        }
        //appendComplete:
        memset(finalBuffer, 0, BUFFER_LENGTH);
        //printf("Made it future.\n");
        write(connfd, goodResponse, strlen(goodResponse));

        memset(goodResponse, 0, BUFFER_LENGTH);
        //close(activeFile);
        break;
    default: break;
    }
    memset(requestBuffer, 0, BUFFER_LENGTH);
    (void) connfd;
    return;
    /*
    if (currentState != REQUEST_COMPLETE) {
        switch (requestType) {
            case UNIMPLEMENTED_REQUEST:
                sprintf(requestError, "HTTP/1.1 501 Not Implemented\r\nContent-Length: %ld\r\n\r\nNot Implemented\n", strlen("Not Implemented\n"));
                m = write(connfd, requestError, sizeof(requestError));
                break;
            case BAD_REQUEST:
                sprintf(requestError, "HTTP/1.1 400 Bad Request\r\nContent-Length: %ld\r\n\r\nBad Request\n", strlen("Bad Request\n"));
                m = write(connfd, requestError, sizeof(requestError));
                break;
            default:
                sprintf(requestError, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: %ld\r\n\r\nInternal Server Error\n", strlen("Internal Server Error\n"));
                m = write(connfd, requestError, sizeof(requestError));
                break;
        }
        memset(requestBuffer, 0,  BUFFER_LENGTH);
        return;
    }
    */

    //while ((substring == strtok_r(remainder, " ", &remainder))) {
    //    printf("%s\n", substring);
    /*
        if (substring[0] == 'G' && substring[1] == 'E' && substring[2] == 'T' && substring[3] == ' ') {

        }
        else if (substring[0] == 'P' && substring[1] == 'U' && substring[2] == 'T' && substring[3] == ' ') {

        }
        else if (substring[0] == 'A' && substring[1] == 'P' && substring[2] == 'P' && substring[3] == 'E' && substring[4] == 'N' && substring[5] == 'D' && substring[6] == ' ') {

        }
        */
    //}
    //}
    //}
}

int main(int argc, char *argv[]) {
    int listenfd;
    uint16_t port;

    if (argc != 2) {
        errx(EXIT_FAILURE, "wrong arguments: %s port_num", argv[0]);
    }

    port = strtouint16(argv[1]);
    if (port == 0) {
        errx(EXIT_FAILURE, "invalid port number: %s", argv[1]);
    }
    listenfd = create_listen_socket(port);

    signal(SIGPIPE, SIG_IGN);

    while (1) {
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            warn("accept error");
            continue;
        }
        handle_connection(connfd);

        // good code opens and closes objects in the same context. *sigh*
        close(connfd);
    }

    return EXIT_SUCCESS;
}
