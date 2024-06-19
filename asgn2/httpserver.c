#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define OPTIONS              "t:l:"
#define BUF_SIZE             4096
#define DEFAULT_THREAD_COUNT 4

static FILE *logfile;
#define LOG(...) fprintf(logfile, __VA_ARGS__);

// Converts a string to an 16 bits unsigned integer.
// Returns 0 if the string is malformed or out of the range.
static size_t strtouint16(char number[]) {
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

// Creates a socket for listening for connections.
// Closes the program and prints an error message on error.
static int create_listen_socket(uint16_t port) {
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
    if (listen(listenfd, 128) < 0) {
        err(EXIT_FAILURE, "listen error");
    }
    return listenfd;
}

#define BAD_REQUEST           0
#define GET_REQUEST           1
#define PUT_REQUEST           2
#define APPEND_REQUEST        3
#define UNIMPLEMENTED_REQUEST 4
#define NOT_FOUND             6
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
#define ID_SEEKING       5
#define BODY_SEEKING     6
#define REQUEST_COMPLETE 7

#define NOT_EXPECTED      0
#define CONTINUE_EXPECTED 1

#define NO_RESPONSE        0
#define OK_RESPONSE        1
#define CREATED_RESPONSE   2
#define NOT_FOUND_RESPONSE 3
#define FAIL_RESPONSE      4

#define BUFFER_LENGTH BUF_SIZE

#define NO_MORE_READING 0
#define KEEP_READING    1

void audit(int oper, char *uri, int status, int requestid) {
    //printf("%d,%s,%d,%d\n", oper, uri, status, requestid);
    char output[2048] = { 0 };
    char request[9] = { 0 };
    int response = 0;

    switch (oper) {
    case GET_REQUEST: sprintf(request, "GET"); break;
    case PUT_REQUEST: sprintf(request, "PUT"); break;
    case APPEND_REQUEST: sprintf(request, "APPEND"); break;
    default: sprintf(request, "FAIL"); break;
    }
    switch (status) {
    case OK_RESPONSE: response = 200; break;
    case CREATED_RESPONSE: response = 201; break;
    case NOT_FOUND_RESPONSE: response = 404; break;
    case FAIL_RESPONSE: response = 500; break;
    default: response = 500; break;
    }
    //printf("%s,%s,%d,%d!!\n", request, uri, response, requestid);
    LOG("%s,%s,%d,%d\n", request, uri, response, requestid);
    fflush(logfile);
    return;
}

void failHandler(int connfd, int errorType, int requestType, char *uri, int requestID) {
    int j = 0;
    char openError[BUFFER_LENGTH] = { 0 };
    switch (errorType) {
    case ENOENT:
        snprintf(openError, BUFFER_LENGTH,
            "HTTP/1.1 404 Not Found\r\nContent-Length: %ld\r\n\r\nNot Found\n",
            strlen("Not Found\n"));
        j = write(connfd, openError, strlen(openError));
        audit(requestType, uri, NOT_FOUND_RESPONSE, requestID);
        break;
    default:
        snprintf(openError, BUFFER_LENGTH,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: %ld\r\n\r\nInternal Server "
            "Error\n",
            strlen("Internal Server Error\n"));
        j = write(connfd, openError, strlen(openError));
        audit(requestType, uri, FAIL_RESPONSE, requestID);
        break;
    }
    memset(openError, 0, BUFFER_LENGTH);
    close(connfd);
    return;
}

#define checkReadWrite(valueRW, type)                                                              \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            if (type == PUT_REQUEST) {                                                             \
                goto putComplete;                                                                  \
            } else if (type == APPEND_REQUEST) {                                                   \
                goto appendComplete;                                                               \
            } else if (type == GET_REQUEST) {                                                      \
                break;                                                                             \
            }                                                                                      \
        } else {                                                                                   \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
            memset(requestBuffer, 0, BUFFER_LENGTH);                                               \
            return;                                                                                \
        }                                                                                          \
    }

#define TIMEOUT_CHECK(valueRW)                                                                     \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
            return;                                                                                \
        }                                                                                          \
    }

#define TIMEOUT_CHECK_LOOP(valueRW)                                                                \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            break;                                                                                 \
        } else {                                                                                   \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
            memset(requestBuffer, 0, BUFFER_LENGTH);                                               \
            return;                                                                                \
        }                                                                                          \
    }
#define checkReadWriteAppend(valueRW)                                                              \
    if (valueRW < 0) {                                                                             \
        failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);                  \
        memset(requestBuffer, 0, BUFFER_LENGTH);                                                   \
        return;                                                                                    \
    }

#define evaluateWrite(written, type)                                                               \
    {                                                                                              \
        if (written >= contentLength) {                                                            \
            if (type == PUT_REQUEST) {                                                             \
                goto putComplete;                                                                  \
            } else if (type == APPEND_REQUEST) {                                                   \
                goto appendComplete;                                                               \
            }                                                                                      \
        }                                                                                          \
    }

static void handle_connection(int connfd) {
    int currentState = METHOD_SEEKING;
    int requestType = BAD_REQUEST;
    int uriStatus = URI_LOST;
    int versionStatus = NO_VERSION;
    int newFileStatus = NO_NEW_FILE;
    uint32_t contentLength = 0;
    uint32_t IDLength = 0;
    int readingStatus = NO_MORE_READING;

    char requestBuffer[BUFFER_LENGTH] = { 0 };
    char requestCopy[BUFFER_LENGTH] = { 0 };
    char finalBuffer[BUFFER_LENGTH] = { 0 };
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
    char *trueIDPointer;
    char trueIDLength[10] = { 0 };
    char *trueIDToken;
    int expectedContinue = NOT_EXPECTED;

    struct timeval tv;

    int n = read(connfd, requestBuffer, BUFFER_LENGTH - 1);
    if (n < 0) {

        return;
    }
    /*int n;
    char requestBuffer[BUF_SIZE] = {0};
    ssize_t bytes_read, bytes_written, bytes;
    do {
        // Read from connfd until EOF or error.
        n = read(connfd, requestBuffer, sizeof(requestBuffer));
        if (n < 0) {
            return;
        }
    } while (n > 0);*/
    char *tempBody;
    char *body = requestBuffer;

    strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
    char *remainder = requestCopy;
    substring = strtok_r(remainder, " ", &remainder);
    if (substring == NULL) {
        //printf("oh no!!\n");
        memset(requestBuffer, 0, BUFFER_LENGTH);
        //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
        return;
    }
    //printf("Got here\n");
    while (currentState != REQUEST_COMPLETE) {
        switch (currentState) {
        case METHOD_SEEKING:
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
            } else if (strstr(substring, "APPEND") != NULL) {
                if (strlen(substring) == 6) {
                    requestType = APPEND_REQUEST;
                } else {
                    requestType = UNIMPLEMENTED_REQUEST;
                }
            } else {
                requestType = UNIMPLEMENTED_REQUEST;
            }
            if (requestType == UNIMPLEMENTED_REQUEST || requestType == BAD_REQUEST) {
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            }
            substring = strtok_r(NULL, " ", &remainder);
            if (substring == NULL) {
                failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
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
                    //strncpy(betterURIString, uriString, sizeof(betterURIString));
                    //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
                    memset(requestBuffer, 0, BUFFER_LENGTH);
                    return;
                }
            }
            substring = strtok_r(NULL, "\r\n", &remainder);

            currentState = VERSION_SEEKING;
            break;
        case VERSION_SEEKING:

            versionStatus = VERSION_FOUND;

            while (substring != NULL) {
                substring = strtok_r(NULL, "END", &remainder);
            }
            if (requestType != GET_REQUEST && requestType != BAD_REQUEST
                && requestType != UNIMPLEMENTED_REQUEST) {
                currentState = LENGTH_SEEKING;
            } else {
                currentState = ID_SEEKING;
            }
            //printf("Found the version.\n");
            break;
        case LENGTH_SEEKING:

            memset(requestCopy, 0, BUFFER_LENGTH);

            strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);

            trueContentPointer = strstr(requestCopy, "\r\nContent-Length: ");

            trueContentToken = trueContentPointer;
            trueContentPointer = strtok_r(trueContentToken, " ", &trueContentToken);
            trueContentPointer = strtok_r(NULL, "\r\n", &trueContentToken);
            //printf(trueContentPointer);
            if (trueContentToken != NULL) {
                strncpy(trueContentLength, trueContentPointer, 10);
            }

            contentLength = strtouint32(trueContentLength);
            //printf("Got the length %d\n", contentLength);

            while (trueContentPointer != NULL) {
                trueContentPointer = strtok_r(NULL, "END", &trueContentToken);
            }
            currentState = ID_SEEKING;
            //printf("Found the length\n");
            break;
        case ID_SEEKING:
            IDLength = 0;
            memset(requestCopy, 0, BUFFER_LENGTH);
            strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
            trueIDPointer = strstr(requestCopy, "\r\nRequest-Id: ");
            if (trueIDPointer == NULL) {
                IDLength = 0;
                currentState = REQUEST_COMPLETE;
                break;
            }

            trueIDToken = trueIDPointer;
            trueIDPointer = strtok_r(trueIDToken, " ", &trueIDToken);
            trueIDPointer = strtok_r(NULL, "\r\n", &trueIDToken);
            //printf(trueContentPointer);
            if (trueIDToken != NULL) {
                strncpy(trueIDLength, trueIDPointer, 10);
            }

            IDLength = strtouint32(trueIDLength);

            while (trueIDPointer != NULL) {
                trueIDPointer = strtok_r(NULL, "END", &trueIDToken);
            }

            currentState = REQUEST_COMPLETE;
            //printf("Made it through\n");
            break;
        default: break;
        }
    }
    memset(requestCopy, 0, BUFFER_LENGTH);
    //printf("Survived the switch\n");
    if (strstr(requestBuffer, "Expect: 100-continue") != NULL) {
        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 100 Continue\r\n");
        expectedContinue = CONTINUE_EXPECTED;
    }
    if (requestType == UNIMPLEMENTED_REQUEST || requestType == BAD_REQUEST) {
        //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
        memset(requestBuffer, 0, BUFFER_LENGTH);
        return;
    }
    int activeFile;
    int openStatus;
    int rwStatus = 0;
    int size = 0;
    struct stat st;
    if (stat(&betterURIString[1], &st) == -1) {
    } else {
        size = st.st_size;
    }
    int status = 0;
    char *cursedPointer;
    int rollingContent = 0;
    int trueHeaderLength = 0;
    int totalWritten = 0;
    int tempValue = 0;
    switch (requestType) {
    case GET_REQUEST:
        memset(goodResponse, 0, BUFFER_LENGTH);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        //setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (access(&betterURIString[1], R_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_RDONLY);

        openStatus = errno;
        if (activeFile == -1) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        status = 200;
        snprintf(
            goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", size);
        audit(requestType, betterURIString, OK_RESPONSE, IDLength);
        write(connfd, goodResponse, strlen(goodResponse));
        int p = read(activeFile, finalBuffer, BUFFER_LENGTH);
        checkReadWrite(p, requestType);
        while (p != 0) {
            rwStatus = write(connfd, finalBuffer, p);
            checkReadWrite(rwStatus, requestType);
            memset(finalBuffer, 0, p);

            p = read(activeFile, finalBuffer, BUFFER_LENGTH);
            checkReadWrite(p, requestType);
        }
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    case PUT_REQUEST:
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            if (errno != ENOENT) {
                failHandler(connfd, errno, requestType, betterURIString, IDLength);
                memset(requestBuffer, 0, BUFFER_LENGTH);
                return;
            } else {
                newFileStatus = FILE_CREATED;
            }
        }
        if ((access(&betterURIString[1], W_OK) != 0) && (newFileStatus != FILE_CREATED)) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        openStatus = errno;
        if (activeFile == -1) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        tempBody = strstr(requestBuffer, endofHeader);
        cursedPointer = tempBody + strlen("\r\n\r\n");
        rollingContent = contentLength;
        trueHeaderLength = cursedPointer - &requestBuffer[0];
        //printf("%d vs %d\n", trueHeaderLength, n);
        if (trueHeaderLength == n && contentLength > 0) {
            expectedContinue = CONTINUE_EXPECTED;
        }
        if (expectedContinue == CONTINUE_EXPECTED) {
            memset(goodResponse, 0, BUFFER_LENGTH);
            n = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWrite(n, requestType);
            if (n < contentLength) {
                totalWritten += write(activeFile, finalBuffer, n);
                evaluateWrite(totalWritten, requestType);
            } else {
                totalWritten += write(activeFile, finalBuffer, contentLength);
                goto putComplete;
            }
            readingStatus = KEEP_READING;
            goto expectedContinueStart;
        } else {
            memset(goodResponse, 0, BUFFER_LENGTH);
        }

        if (tempBody) {
            if (n - trueHeaderLength > contentLength) {
                totalWritten += write(activeFile, cursedPointer, contentLength);
                //write(connfd, goodResponse, strlen(goodResponse));
                //memset(finalBuffer, 0, BUFFER_LENGTH);
                //memset(goodResponse, 0, BUFFER_LENGTH);
                goto putComplete;
            } else {
                tempValue = write(activeFile, cursedPointer, n - trueHeaderLength);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                rollingContent = contentLength - tempValue;
                readingStatus = KEEP_READING;
            }
        }
        tempBody = NULL;
        cursedPointer = NULL;
    expectedContinueStart:
        memset(requestBuffer, 0, BUFFER_LENGTH);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int q;
        if (readingStatus == KEEP_READING) {
            q = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            //printf("Length of Q: %d\n", q);
            checkReadWrite(q, requestType);
            while (q > 0) {
                if (q > rollingContent) {
                    totalWritten += write(activeFile, finalBuffer, rollingContent);
                    evaluateWrite(totalWritten, requestType);
                    //checkReadWrite(rwStatus, requestType);
                    break;
                }
                tempValue = write(activeFile, finalBuffer, q);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                //checkReadWrite(rwStatus, requestType);
                rollingContent = rollingContent - tempValue;

                memset(finalBuffer, 0, BUFFER_LENGTH);

                q = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
                TIMEOUT_CHECK_LOOP(q);
            }
        }
    putComplete:
        memset(goodResponse, 0, BUFFER_LENGTH);
        if (newFileStatus == FILE_CREATED) {
            snprintf(goodResponse, BUFFER_LENGTH,
                "HTTP/1.1 201 Created\r\nContent-Length: %ld\r\n\r\nCreated\n",
                strlen("Created\n"));
            status = CREATED_RESPONSE;
        } else {
            snprintf(goodResponse, BUFFER_LENGTH,
                "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n", strlen("OK\n"));
            status = OK_RESPONSE;
        }
        //printf("%s\n",goodResponse);
        write(connfd, goodResponse, strlen(goodResponse));
        audit(requestType, betterURIString, status, IDLength);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    case APPEND_REQUEST:
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        activeFile = open(&betterURIString[1], O_RDWR | O_APPEND);
        openStatus = errno;
        if (activeFile == -1) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            return;
        }
        tempBody = strstr(requestBuffer, endofHeader);
        cursedPointer = tempBody + strlen("\r\n\r\n");
        rollingContent = contentLength;
        trueHeaderLength = cursedPointer - &requestBuffer[0];
        //printf("%d vs %d\n", trueHeaderLength, n);
        if (trueHeaderLength == n && contentLength > 0) {
            expectedContinue = CONTINUE_EXPECTED;
        }
        if (expectedContinue == CONTINUE_EXPECTED) {
            memset(goodResponse, 0, BUFFER_LENGTH);
            n = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWrite(n, requestType);
            if (n < contentLength) {
                totalWritten += write(activeFile, finalBuffer, n);
                evaluateWrite(totalWritten, requestType);
            } else {
                totalWritten += write(activeFile, finalBuffer, contentLength);
                goto appendComplete;
            }
            readingStatus = KEEP_READING;
            goto expectedContinueStartAppend;
        } else {
            memset(goodResponse, 0, BUFFER_LENGTH);
        }

        if (tempBody) {
            if (n - trueHeaderLength > contentLength) {
                write(activeFile, cursedPointer, contentLength);
                //write(connfd, goodResponse, strlen(goodResponse));
                //memset(finalBuffer, 0, BUFFER_LENGTH);
                //memset(goodResponse, 0, BUFFER_LENGTH);
                goto appendComplete;
            } else {
                tempValue = write(activeFile, cursedPointer, n - trueHeaderLength);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                rollingContent = contentLength - tempValue;
                readingStatus = KEEP_READING;
            }
        }
        tempBody = NULL;
        cursedPointer = NULL;
    expectedContinueStartAppend:
        memset(requestBuffer, 0, BUFFER_LENGTH);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int r;
        if (readingStatus == KEEP_READING) {
            r = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
            //printf("Length of Q: %d\n", q);
            checkReadWrite(r, requestType);
            while (r > 0) {
                if (r > rollingContent) {
                    totalWritten += write(activeFile, finalBuffer, rollingContent);
                    evaluateWrite(totalWritten, requestType);
                    //checkReadWrite(rwStatus, requestType);
                    break;
                }
                tempValue = write(activeFile, finalBuffer, r);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                //checkReadWrite(rwStatus, requestType);
                rollingContent = rollingContent - tempValue;

                memset(finalBuffer, 0, BUFFER_LENGTH);

                r = read(connfd, finalBuffer, BUFFER_LENGTH - 1);
                TIMEOUT_CHECK_LOOP(r);
            }
        }
    appendComplete:
        memset(goodResponse, 0, BUFFER_LENGTH);

        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n",
            strlen("OK\n"));
        status = OK_RESPONSE;

        write(connfd, goodResponse, strlen(goodResponse));
        audit(requestType, betterURIString, status, IDLength);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    default: break;
    }
    memset(requestBuffer, 0, BUFFER_LENGTH);
    (void) connfd;
    return;

    /*
    char buf[BUF_SIZE];
    ssize_t bytes_read, bytes_written, bytes;
    do {
        // Read from connfd until EOF or error.
        bytes_read = read(connfd, buf, sizeof(buf));
        if (bytes_read < 0) {
            return;
        }

        // Write to stdout.
        bytes = 0;
        do {
            bytes_written = write(STDOUT_FILENO, buf + bytes, bytes_read - bytes);
            if (bytes_written < 0) {
                return;
            }
            bytes += bytes_written;
        } while (bytes_written > 0 && bytes < bytes_read);

        // Write to connfd.
        bytes = 0;
        do {
            bytes_written = write(connfd, buf + bytes, bytes_read - bytes);
            if (bytes_written < 0) {
                return;
            }
            bytes += bytes_written;
        } while (bytes_written > 0 && bytes < bytes_read);
    } while (bytes_read > 0);*/
}

static void sigterm_handler(int sig) {
    if (sig == SIGTERM) {
        warnx("received SIGTERM");
        fclose(logfile);
        exit(EXIT_SUCCESS);
    }
}

static void sigint_handler(int sig) {
    if (sig == SIGINT) {
        warnx("received SIGINT");
        fclose(logfile);
        exit(EXIT_SUCCESS);
    }
}

static void usage(char *exec) {
    fprintf(stderr, "usage: %s [-t threads] [-l logfile] <port>\n", exec);
}

int main(int argc, char *argv[]) {
    int opt = 0;
    int threads = DEFAULT_THREAD_COUNT;
    logfile = stderr;

    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            threads = strtol(optarg, NULL, 10);
            if (threads <= 0) {
                errx(EXIT_FAILURE, "bad number of threads");
            }
            break;
        case 'l':
            logfile = fopen(optarg, "w");
            if (!logfile) {
                errx(EXIT_FAILURE, "bad logfile");
            }
            break;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        warnx("wrong number of arguments");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t port = strtouint16(argv[optind]);
    if (port == 0) {
        errx(EXIT_FAILURE, "bad port number: %s", argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigint_handler);

    int listenfd = create_listen_socket(port);
    //LOG("port=%" PRIu16 ", threads=%d\n", port, threads);

    for (;;) {
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            warn("accept error");
            continue;
        }
        handle_connection(connfd);
        close(connfd);
    }

    return EXIT_SUCCESS;
}
