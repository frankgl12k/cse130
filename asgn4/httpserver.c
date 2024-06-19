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
#include <pthread.h>
#include <sys/epoll.h>
#include <search.h>
#include <poll.h>
#include <semaphore.h>

#define OPTIONS                "t:l:"
#define BUF_SIZE               4096
#define DEFAULT_THREAD_COUNT   4
#define ARBITRARY_THREAD_QUEUE 10001
#define SIG_DIE_THREAD         11037
#define QUEUE_SIZE             ARBITRARY_THREAD_QUEUE

static FILE *logfile;
#define LOG(...) fprintf(logfile, __VA_ARGS__);

pthread_mutex_t queueMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t uriMTX = PTHREAD_MUTEX_INITIALIZER;

#define TABLE_SIZE 255
int table;

sem_t fileLock[ARBITRARY_THREAD_QUEUE];
sem_t readLock[ARBITRARY_THREAD_QUEUE];
int readers[ARBITRARY_THREAD_QUEUE] = { 0 };
int globalFileCount = 0;

pthread_cond_t writeQueue = PTHREAD_COND_INITIALIZER;
pthread_cond_t readQueue = PTHREAD_COND_INITIALIZER;
int addQueue = 0;
int subQueue = 0;
int numInQueue = 0;
int destroyAllThreads = 0;

typedef struct connObj {
    int connfd;
    int currentState;
    int requestType;
    int uriStatus;
    int versionStatus;
    int newFileStatus;
    uint32_t contentLength;
    uint32_t IDLength;
    int lengthStatus;
    int idStatus;
    int readingStatus;
    int numRead;
    int numWrite;
    char betterURIString[20];
    int lastCheckpoint;
    int rollingContent;
    int trueHeaderLength;
    int totalWritten;
    int processingFinished;
    int tempFile;
    char tempURIString[32];
    int rwLock;
} connObj;

connObj connections[QUEUE_SIZE];
int superThreads = 0;
pthread_t *thread;

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

typedef struct threadfd {
    int connfd;
    int position;
} threadfd;

void audit(int oper, char *uri, int status, int requestid) {
    //printf"%d,%s,%d,%d\n", oper, uri, status, requestid);
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
    //printf"%s,%s,%d,%d!!\n", request, uri, response, requestid);
    pthread_mutex_lock(&logMTX);
    LOG("%s,%s,%d,%d\n", request, uri, response, requestid);
    fflush(logfile);
    pthread_mutex_unlock(&logMTX);
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
    //
    return;
}

#define exitGet(globalValue, readerValue)                                                          \
    sem_wait(&readLock[globalValue]);                                                              \
    readerValue = readerValue - 1;                                                                 \
    if (readerValue == 0) {                                                                        \
        sem_post(&fileLock[globalValue]);                                                          \
    }                                                                                              \
    sem_post(&readLock[globalValue]);

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
            if (type == GET_REQUEST) {                                                             \
                exitGet(rwLock, readers[rwLock]);                                                  \
            }                                                                                      \
            if ((type == PUT_REQUEST || type == APPEND_REQUEST) && (activeFile != 0)) {            \
                sem_post(&fileLock[rwLock]);                                                       \
            }                                                                                      \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
            memset(requestBuffer, 0, BUFFER_LENGTH);                                               \
                                                                                                   \
            goto restartThread;                                                                    \
        }                                                                                          \
    }

#define TIMEOUT_CHECK(valueRW)                                                                     \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
                                                                                                   \
            goto restartThread;                                                                    \
        }                                                                                          \
    }

#define TIMEOUT_CHECK_LOOP(valueRW)                                                                \
    if (valueRW < 0) {                                                                             \
        if (errno == EWOULDBLOCK) {                                                                \
            break;                                                                                 \
        } else {                                                                                   \
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);              \
            memset(requestBuffer, 0, BUFFER_LENGTH);                                               \
                                                                                                   \
            goto restartThread;                                                                    \
        }                                                                                          \
    }
#define checkReadWriteAppend(valueRW)                                                              \
    if (valueRW < 0) {                                                                             \
        failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);                  \
        memset(requestBuffer, 0, BUFFER_LENGTH);                                                   \
                                                                                                   \
        goto restartThread;                                                                        \
    }

#define evaluateWrite(written, type)                                                               \
    {                                                                                              \
        if (written >= contentLength) {                                                            \
            if (type == PUT_REQUEST) {                                                             \
                goto tempComplete;                                                                 \
            } else if (type == APPEND_REQUEST) {                                                   \
                goto tempComplete;                                                                 \
            }                                                                                      \
        }                                                                                          \
    }

#define updateObject(object, checkpt)                                                              \
    {                                                                                              \
        object.lastCheckpoint = checkpt;                                                           \
        object.processingFinished = currentProcess;                                                \
        if (checkpoint != initialRead) {                                                           \
            strcpy(object.betterURIString, betterURIString);                                       \
            object.rwLock = rwLock;                                                                \
            object.currentState = currentState;                                                    \
            object.requestType = requestType;                                                      \
            object.uriStatus = uriStatus;                                                          \
            object.versionStatus = versionStatus;                                                  \
            object.newFileStatus = newFileStatus;                                                  \
            object.numRead = n;                                                                    \
            object.numWrite = N;                                                                   \
            object.lengthStatus = lengthStatus;                                                    \
            object.idStatus = idStatus;                                                            \
            switch (currentState) {                                                                \
            case LENGTH_SEEKING:                                                                   \
                if (requestType == GET_REQUEST) {                                                  \
                    object.currentState = ID_SEEKING;                                              \
                }                                                                                  \
                break;                                                                             \
            case ID_SEEKING:                                                                       \
                if (requestType != GET_REQUEST && lengthStatus != MISSING_LENGTH) {                \
                    object.contentLength = contentLength;                                          \
                }                                                                                  \
                break;                                                                             \
            default:                                                                               \
                object.contentLength = contentLength;                                              \
                object.IDLength = IDLength;                                                        \
                object.readingStatus = readingStatus;                                              \
                object.rollingContent = rollingContent;                                            \
                object.trueHeaderLength = trueHeaderLength;                                        \
                object.totalWritten = totalWritten;                                                \
                object.tempFile = tempFile;                                                        \
                strcpy(object.tempURIString, tempURIString);                                       \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
    }

#define restoreObject(object, checkpt)                                                             \
    {                                                                                              \
        checkpt = object.lastCheckpoint;                                                           \
        currentProcess = object.processingFinished;                                                \
        if (checkpoint != initialRead) {                                                           \
            strcpy(betterURIString, object.betterURIString);                                       \
            rwLock = object.rwLock;                                                                \
            currentState = object.currentState;                                                    \
            requestType = object.requestType;                                                      \
            uriStatus = object.uriStatus;                                                          \
            versionStatus = object.versionStatus;                                                  \
            newFileStatus = object.newFileStatus;                                                  \
            n = object.numRead;                                                                    \
            N = object.numWrite;                                                                   \
            lengthStatus = object.lengthStatus;                                                    \
            idStatus = object.idStatus;                                                            \
            switch (object.currentState) {                                                         \
            case LENGTH_SEEKING:                                                                   \
                if (object.requestType == GET_REQUEST) {                                           \
                    currentState = ID_SEEKING;                                                     \
                }                                                                                  \
                break;                                                                             \
            case ID_SEEKING:                                                                       \
                if (object.requestType != GET_REQUEST && object.lengthStatus != MISSING_LENGTH) {  \
                    contentLength = object.contentLength;                                          \
                }                                                                                  \
                break;                                                                             \
            default:                                                                               \
                contentLength = object.contentLength;                                              \
                IDLength = object.IDLength;                                                        \
                readingStatus = object.readingStatus;                                              \
                rollingContent = object.rollingContent;                                            \
                trueHeaderLength = object.trueHeaderLength;                                        \
                totalWritten = object.totalWritten;                                                \
                tempFile = object.tempFile;                                                        \
                strcpy(tempURIString, object.tempURIString);                                       \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
    }

#define resetObject(object)                                                                        \
    {                                                                                              \
        object.lastCheckpoint = initialRead;                                                       \
        object.processingFinished = NEW_PROCESS;                                                   \
        memset(object.betterURIString, 0, 20);                                                     \
        object.rwLock = 0;                                                                         \
        object.currentState = METHOD_SEEKING;                                                      \
        object.requestType = BAD_REQUEST;                                                          \
        object.uriStatus = URI_LOST;                                                               \
        object.versionStatus = NO_VERSION;                                                         \
        object.newFileStatus = NO_NEW_FILE;                                                        \
        object.lengthStatus = MISSING_LENGTH;                                                      \
        object.idStatus = MISSING_ID;                                                              \
        object.numRead = 0;                                                                        \
        object.numWrite = 0;                                                                       \
        object.contentLength = 0;                                                                  \
        object.IDLength = 0;                                                                       \
        object.readingStatus = NO_MORE_READING;                                                    \
        object.rollingContent = 0;                                                                 \
        object.trueHeaderLength = 0;                                                               \
        object.totalWritten = 0;                                                                   \
        object.tempFile = 0;                                                                       \
        memset(object.tempURIString, 0, 32);                                                       \
    }

#define jumpRegister(checkpt)                                                                      \
    {                                                                                              \
        switch (checkpt) {                                                                         \
        case initialRead: goto initialReadLabel; break;                                            \
        case lengthSeeking: goto lengthSeekingLabel; break;                                        \
        case idSeeking: goto idSeekingLabel; break;                                                \
        case putExpectedContinue: goto putExpectedContinueLabel; break;                            \
        case putKeepReading: goto putKeepReadingLabel; break;                                      \
        case putLoopReading: goto putLoopReadingLabel; break;                                      \
        case appendExpectedContinue: goto appendExpectedContinueLabel; break;                      \
        case appendKeepReading: goto appendKeepReadingLabel; break;                                \
        case appendLoopReading: goto appendLoopReadingLabel; break;                                \
        case tempExpectedContinue: goto tempExpectedContinueLabel; break;                          \
        case tempKeepReading: goto tempKeepReadingLabel; break;                                    \
        case tempLoopReading: goto tempLoopReadingLabel; break;                                    \
        default: goto initialReadLabel; break;                                                     \
        }                                                                                          \
    }

#define reAddConnfd(object, checkpt)                                                               \
    {                                                                                              \
        pthread_mutex_lock(&queueMTX);                                                             \
        while (numInQueue == QUEUE_SIZE) {                                                         \
            pthread_cond_wait(&writeQueue, &queueMTX);                                             \
        }                                                                                          \
        updateObject(object, checkpt);                                                             \
        connections[addQueue].connfd = connfd;                                                     \
        addQueue = (addQueue + 1) % QUEUE_SIZE;                                                    \
        numInQueue++;                                                                              \
        pthread_mutex_unlock(&queueMTX);                                                           \
        pthread_cond_signal(&readQueue);                                                           \
    }

#define END_THREAD                                                                                 \
    {                                                                                              \
        if (destroyAllThreads == 1) {                                                              \
            return 0;                                                                              \
        }                                                                                          \
    }

enum checkpointList {
    initialRead,
    lengthSeeking,
    idSeeking,
    putExpectedContinue,
    putKeepReading,
    putLoopReading,
    appendExpectedContinue,
    appendKeepReading,
    appendLoopReading,
    tempExpectedContinue,
    tempKeepReading,
    tempLoopReading
};

#define NEW_PROCESS  0
#define NOT_FINISHED 1
#define FINISHED     2

#define NOT_READY 0
#define READY     1

#define LOCKED_GATE      0
#define NEVER_ENTER_GATE 1

#define MISSING_LENGTH 0
#define GOT_LENGTH     1
#define TRIED_LENGTH   2

#define MISSING_ID 0
#define GOT_ID     1
#define TRIED_ID   2

#define POLL(thisPoll, object, checkpoint)                                                         \
    poll(thisPoll, 1, 3000);                                                                       \
    if (thisPoll[0].revents != POLLIN) {                                                           \
        updateObject(object, checkpoint);                                                          \
        reAddConnfd(object, checkpoint);                                                           \
        goto restartThread;                                                                        \
    }

#define RESTORE(object, checkpt)                                                                   \
    {                                                                                              \
        restoreObject(object, checkpt);                                                            \
        jumpRegister(object.lastCheckpoint);                                                       \
    }
//typedef struct pollfd {
//               int   fd;         /* file descriptor */
//               short events;     /* requested events */
//               short revents;    /* returned events */
//           }pollfd;

static void *handle_connection(void *ptr) {
    //printf("%ld\n", pthread_self());

    /*item.key = "none";
    item.data = (void *) 128;
    lookItem = hsearch(item, FIND);
    if (lookItem == NULL) {
        printf("Null!\n");
    }
    else {
        printf("Found!\n");
    }
    item.key = "test";
    lookItem = hsearch(item, FIND);
    if (lookItem != NULL) {
        printf("Found it! %ld\n", (uintptr_t) lookItem->data);
    }
    else {
        printf("Not found!\n");
    }*/

    ENTRY item;
    ENTRY *lookItem;
    int connfd;

    int currentState = METHOD_SEEKING;
    int requestType = BAD_REQUEST;
    int uriStatus = URI_LOST;
    int versionStatus = NO_VERSION;
    int newFileStatus = NO_NEW_FILE;
    uint32_t contentLength = 0;
    uint32_t IDLength = 0;
    int readingStatus = NO_MORE_READING;
    int n = 0;
    int N = 0;
    int rollingContent = 0;

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
    int currentProcess = NOT_FINISHED;
    int trueHeaderLength = 0;
    int totalWritten = 0;
    int oneWayGate = LOCKED_GATE;
    int lengthStatus = MISSING_LENGTH;
    int idStatus = MISSING_ID;
    int tempFile = 0;
    char tempURIString[32] = { 0 };
    int rwLock = 0;

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int connReady = NOT_READY;

    struct pollfd thisPoll[1];

    thisPoll[0].events = POLLIN;
    connObj thisConn;
restartThread:
    connfd = 0;
    enum checkpointList checkpoint = initialRead;

    END_THREAD;

    pthread_mutex_lock(&queueMTX);
    while (numInQueue == 0) {
        pthread_cond_wait(&readQueue, &queueMTX);
    }

    //setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    connfd = connections[subQueue].connfd;
    if (connections[subQueue].processingFinished == FINISHED) {
        resetObject(connections[subQueue]);
    } else {
        restoreObject(thisConn, checkpoint);
    }

    subQueue = (subQueue + 1) % QUEUE_SIZE;
    numInQueue--;
    pthread_mutex_unlock(&queueMTX);
    pthread_cond_signal(&writeQueue);

    END_THREAD;

    thisPoll[0].fd = connfd;
    //printf("this: %s and %d and %d and %d and %d and %d\n", &betterURIString[0], thisConn.tempFile,
    //    thisConn.lastCheckpoint, requestType, uriStatus, versionStatus);

    if (thisConn.processingFinished != NEW_PROCESS) {

        RESTORE(thisConn, checkpoint);
    }

    //poll(thisPoll, 1, 1000);
    //if (thisPoll[0].revents != POLLIN) {
    //    reAddConnfd(connfd);
    //    goto restartThread;
    //}

    //threadfd thisConn = *(threadfd *) ptr;
    //int connfd = thisConn.connfd;

    currentState = METHOD_SEEKING;
    requestType = BAD_REQUEST;
    uriStatus = URI_LOST;
    versionStatus = NO_VERSION;
    newFileStatus = NO_NEW_FILE;
    contentLength = 0;
    IDLength = 0;
    readingStatus = NO_MORE_READING;
    n = 0;
    N = 0;
    rollingContent = 0;
    trueHeaderLength = 0;
    totalWritten = 0;
    oneWayGate = LOCKED_GATE;
    lengthStatus = MISSING_LENGTH;
    idStatus = MISSING_ID;
    tempFile = 0;
    rwLock = 0;
    item.key = "no";
    item.data = (void *) 0;

    memset(requestBuffer, 0, BUFFER_LENGTH);
    memset(requestCopy, 0, BUFFER_LENGTH);
    memset(finalBuffer, 0, BUFFER_LENGTH);
    memset(goodResponse, 0, BUFFER_LENGTH);
    memset(requestTypeString, 0, 16);
    memset(uriString, 0, 20);
    memset(betterURIString, 0, 20);
    memset(tempURIString, 0, 32);
    //char endofHeader[] = "\r\n\r\n";
    substring = NULL;
    m = 0;
    trueContentPointer = NULL;

    memset(trueContentLength, 0, 10);
    trueContentToken = NULL;
    trueIDPointer = NULL;

    memset(trueIDLength, 0, 10);
    trueIDToken = NULL;
    expectedContinue = NOT_EXPECTED;
    currentProcess = NOT_FINISHED;

initialReadLabel:
    //printf("did not read stuff!\n");
    POLL(thisPoll, thisConn, checkpoint);
    usleep(1000);

    memset(requestBuffer, 0, BUFFER_LENGTH);
    n = read(connfd, requestBuffer, BUFFER_LENGTH - 1);
    //printf("read stuff! %s\n", requestBuffer);
    //if (n < 0) {
    //    checkpoint = initialRead;
    //    goto restartThread;
    //}
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
        //printf"oh no!!\n");
        memset(requestBuffer, 0, BUFFER_LENGTH);
        //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);

        goto restartThread;
    }
    //printf"Got here\n");
    while (currentState != REQUEST_COMPLETE) {
        switch (currentState) {
        case METHOD_SEEKING:
            if (strstr(substring, "GET") != NULL) {
                if (strlen(substring) == 3) {
                    requestType = GET_REQUEST;
                } else {
                    requestType = UNIMPLEMENTED_REQUEST;
                }
                //printf"Append request!\n");
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

                goto restartThread;
            }
            substring = strtok_r(NULL, " ", &remainder);
            if (substring == NULL) {
                failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
                memset(requestBuffer, 0, BUFFER_LENGTH);

                goto restartThread;
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

                    //printf"better: %s\n", betterURIString);
                } else {
                    //strncpy(betterURIString, uriString, sizeof(betterURIString));
                    //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
                    memset(requestBuffer, 0, BUFFER_LENGTH);

                    goto restartThread;
                }
            }
            substring = strtok_r(NULL, "\r\n", &remainder);

            pthread_mutex_lock(&uriMTX);
            char superTemp[32] = "none";
            memset(superTemp, 0, 32);
            snprintf(superTemp, strlen(&betterURIString[1]), "%s", &betterURIString[1]);
            item.key = superTemp;
            lookItem = hsearch(item, FIND);
            if (lookItem == NULL) {
                //printf("new lock %s\n", superTemp);
                rwLock = globalFileCount;
                globalFileCount = (globalFileCount + 1) % ARBITRARY_THREAD_QUEUE;
                item.key = superTemp;
                item.data = (void *) (intptr_t) rwLock;
                hsearch(item, ENTER);
            } else {
                //printf("old lock\n");
                rwLock = (uintptr_t) lookItem->data;
            }
            pthread_mutex_unlock(&uriMTX);

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
            //printf"Found the version.\n");
            break;
        case LENGTH_SEEKING:
        seekLengthfromID:
            contentLength = 0;
            memset(requestCopy, 0, BUFFER_LENGTH);

            strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
            trueContentPointer = strstr(requestCopy, "\r\nContent-Length: ");
            if (oneWayGate == NEVER_ENTER_GATE) {
            lengthSeekingLabel:
                checkpoint = lengthSeeking;
                POLL(thisPoll, thisConn, checkpoint);
                n = read(connfd, requestBuffer, BUFFER_LENGTH - 1);
                strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
                trueContentPointer = strstr(requestCopy, "Content-Length: ");
                if (trueContentPointer == NULL && idStatus == MISSING_ID) {
                    goto seekIDFromLength;
                } else if (trueContentPointer == NULL
                           && (idStatus == TRIED_ID || idStatus == GOT_ID)) {
                    reAddConnfd(thisConn, checkpoint);
                    goto restartThread;
                }
            }

            if (trueContentPointer == NULL && idStatus == MISSING_ID) {
                idStatus = TRIED_ID;
                goto seekIDFromLength;
            } else if (trueContentPointer == NULL && (idStatus == TRIED_ID || idStatus == GOT_ID)) {
                lengthStatus = MISSING_LENGTH;
                checkpoint = lengthSeeking;
                reAddConnfd(thisConn, checkpoint);
                goto restartThread;
            }

            /*if (trueContentPointer == NULL) {
               //printf"Danger!\n");
                checkpoint = lengthSeeking;
                POLL(thisPoll, thisConn, checkpoint);

                goto restartThread;
            }*/

            trueContentToken = trueContentPointer;
            trueContentPointer = strtok_r(trueContentToken, " ", &trueContentToken);
            trueContentPointer = strtok_r(NULL, "\r\n", &trueContentToken);
            //*print-f(trueContentPointer);
            if (trueContentToken != NULL) {
                strncpy(trueContentLength, trueContentPointer, 10);
            }

            contentLength = strtouint32(trueContentLength);
            //printf"Got the length %d\n", contentLength);

            while (trueContentPointer != NULL) {
                trueContentPointer = strtok_r(NULL, "END", &trueContentToken);
            }
            //printf"Found the length\n");

            lengthStatus = GOT_LENGTH;

            if (idStatus == MISSING_ID) {
                currentState = ID_SEEKING;
            } else if (idStatus == TRIED_ID) {
                idStatus = MISSING_ID;
                checkpoint = idSeeking;
                reAddConnfd(thisConn, checkpoint);
                goto restartThread;
            } else {
                currentState = REQUEST_COMPLETE;
                //printf"Request complete!\n");
            }

            break;
        case ID_SEEKING:
        seekIDFromLength:
            IDLength = 0;

            memset(requestCopy, 0, BUFFER_LENGTH);
            strncpy(requestCopy, requestBuffer, BUFFER_LENGTH);
            trueIDPointer = strstr(requestCopy, "\r\nRequest-Id: ");

            if (oneWayGate == NEVER_ENTER_GATE) {
            idSeekingLabel:
                checkpoint = idSeeking;
                POLL(thisPoll, thisConn, checkpoint);
                n = read(connfd, requestBuffer, BUFFER_LENGTH - 1);
                trueIDPointer = strstr(requestBuffer, "Request-Id: ");
                if (trueIDPointer == NULL && lengthStatus == MISSING_LENGTH) {
                    goto seekLengthfromID;
                } else if (trueIDPointer == NULL
                           && (lengthStatus == TRIED_LENGTH || lengthStatus == GOT_LENGTH)) {
                    reAddConnfd(thisConn, checkpoint);
                    goto restartThread;
                }
            }

            if (trueIDPointer == NULL && lengthStatus == MISSING_LENGTH) {
                lengthStatus = TRIED_LENGTH;
                goto seekLengthfromID;
            } else if (trueIDPointer == NULL
                       && (lengthStatus == TRIED_LENGTH || lengthStatus == GOT_LENGTH)) {
                idStatus = MISSING_ID;
                checkpoint = idSeeking;
                reAddConnfd(thisConn, checkpoint);
                goto restartThread;
            }

            /*if (trueIDPointer == NULL) {
                IDLength = 0;
                checkpoint = idSeeking;
                currentState = REQUEST_COMPLETE;
                break;
            }*/

            trueIDToken = trueIDPointer;
            trueIDPointer = strtok_r(trueIDToken, " ", &trueIDToken);
            trueIDPointer = strtok_r(NULL, "\r\n", &trueIDToken);
            //*print-f(trueContentPointer);
            if (trueIDToken != NULL) {
                strncpy(trueIDLength, trueIDPointer, 10);
            }

            IDLength = strtouint32(trueIDLength);

            while (trueIDPointer != NULL) {
                trueIDPointer = strtok_r(NULL, "END", &trueIDToken);
            }

            idStatus = GOT_ID;

            if (lengthStatus == MISSING_LENGTH && requestType != GET_REQUEST) {
                currentState = LENGTH_SEEKING;
            } else if (lengthStatus == TRIED_LENGTH && requestType != GET_REQUEST) {
                lengthStatus = MISSING_LENGTH;
                checkpoint = lengthSeeking;
                reAddConnfd(thisConn, checkpoint);
                goto restartThread;
            } else if (lengthStatus == GOT_LENGTH || requestType == GET_REQUEST) {
                currentState = REQUEST_COMPLETE;
                //printf"Made it through\n");
            }

            break;
        default: break;
        }
    }
    memset(requestCopy, 0, BUFFER_LENGTH);
    //printf"Survived the switch\n");
    if (requestType == PUT_REQUEST || requestType == APPEND_REQUEST)
        goto tempStart;

    if (strstr(requestBuffer, "Expect: 100-continue") != NULL) {
        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 100 Continue\r\n");
        expectedContinue = CONTINUE_EXPECTED;
    }
    if (requestType == UNIMPLEMENTED_REQUEST || requestType == BAD_REQUEST) {
        //failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
        memset(requestBuffer, 0, BUFFER_LENGTH);

        goto restartThread;
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

    int tempValue = 0;
finalRequest:

    switch (requestType) {
    case GET_REQUEST:
        //printf("get request!\n");
        memset(goodResponse, 0, BUFFER_LENGTH);

        if (access(&betterURIString[1], R_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        sem_wait(&readLock[rwLock]);
        readers[rwLock] = readers[rwLock] + 1;
        if (readers[rwLock] == 1) {
            sem_wait(&fileLock[rwLock]);
        }
        sem_post(&readLock[rwLock]);
        activeFile = open(&betterURIString[1], O_RDONLY);

        openStatus = errno;
        if (activeFile == -1) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);
            exitGet(rwLock, readers[rwLock]);
            goto restartThread;
        }
        status = 200;
        snprintf(
            goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", size);

        write(connfd, goodResponse, strlen(goodResponse));
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int p = read(activeFile, finalBuffer, BUFFER_LENGTH - 1);
        checkReadWrite(p, requestType);
        //printf("infinite?\n");
        while (p != 0) {
            rwStatus = write(connfd, finalBuffer, p);
            checkReadWrite(rwStatus, requestType);
            memset(finalBuffer, 0, BUFFER_LENGTH);

            p = read(activeFile, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWrite(p, requestType);
        }
        //printf("finite\n");
        close(activeFile);
        exitGet(rwLock, readers[rwLock]);
        audit(requestType, betterURIString, OK_RESPONSE, IDLength);
        //printf("no\n");

        //printf("yes!\n");
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    case PUT_REQUEST:
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            if (errno != ENOENT) {
                failHandler(connfd, errno, requestType, betterURIString, IDLength);
                memset(requestBuffer, 0, BUFFER_LENGTH);

                goto restartThread;
            } else {
                newFileStatus = FILE_CREATED;
            }
        }
        if ((access(&betterURIString[1], W_OK) != 0) && (newFileStatus != FILE_CREATED)) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        sem_wait(&fileLock[rwLock]);
        activeFile = open(&betterURIString[1], O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        //printf("um\n");
        openStatus = errno;
        if (activeFile == -1) {
            sem_post(&fileLock[rwLock]);
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        close(tempFile);
        tempFile = open(tempURIString, O_RDONLY);
        unlink(tempURIString);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int q = read(tempFile, finalBuffer, BUFFER_LENGTH - 1);
        //printf("file: %d\n", tempFile);
        //printf("string: %s\n", finalBuffer);
        checkReadWrite(q, requestType);
        while (q != 0) {
            rwStatus = write(activeFile, finalBuffer, q);
            checkReadWrite(rwStatus, requestType);
            memset(finalBuffer, 0, BUFFER_LENGTH);

            q = read(tempFile, finalBuffer, BUFFER_LENGTH - 1);
            checkReadWrite(q, requestType);
        }
        close(activeFile);
        close(tempFile);
    putKeepReadingLabel:
    putLoopReadingLabel:
    putExpectedContinueLabel:
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
        //printf"%s\n", goodResponse);
        sem_post(&fileLock[rwLock]);
        write(connfd, goodResponse, strlen(goodResponse));
        audit(requestType, betterURIString, status, IDLength);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    case APPEND_REQUEST:
        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        if (access(&betterURIString[1], F_OK) != 0) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        sem_wait(&fileLock[rwLock]);
        activeFile = open(&betterURIString[1], O_RDWR | O_APPEND);
        openStatus = errno;
        if (activeFile == -1) {
            sem_wait(&fileLock[rwLock]);
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        close(tempFile);
        tempFile = open(tempURIString, O_RDONLY);
        unlink(tempURIString);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int r = read(tempFile, finalBuffer, BUFFER_LENGTH);
        checkReadWrite(r, requestType);
        while (r != 0) {
            rwStatus = write(activeFile, finalBuffer, r);
            checkReadWrite(rwStatus, requestType);
            memset(finalBuffer, 0, BUFFER_LENGTH);

            r = read(tempFile, finalBuffer, BUFFER_LENGTH);
            checkReadWrite(r, requestType);
        }
        close(activeFile);
        close(tempFile);
    appendKeepReadingLabel:
    appendLoopReadingLabel:
    appendExpectedContinueLabel:
    appendComplete:
        memset(goodResponse, 0, BUFFER_LENGTH);

        snprintf(goodResponse, BUFFER_LENGTH, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\nOK\n",
            strlen("OK\n"));
        status = OK_RESPONSE;
        sem_post(&fileLock[rwLock]);
        write(connfd, goodResponse, strlen(goodResponse));
        audit(requestType, betterURIString, status, IDLength);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
        break;
    default: break;
    }
    memset(requestBuffer, 0, BUFFER_LENGTH);
    close(connfd);
    goto restartThread;

tempStart:
    switch (requestType) {
    default:

        if (strstr(&betterURIString[1], "/") != NULL) {
            failHandler(connfd, BAD_REQUEST, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        if (requestType == PUT_REQUEST) {
            if (access(&betterURIString[1], F_OK) != 0) {
                if (errno != ENOENT) {
                    failHandler(connfd, errno, requestType, betterURIString, IDLength);
                    memset(requestBuffer, 0, BUFFER_LENGTH);

                    goto restartThread;
                } else {
                    newFileStatus = FILE_CREATED;
                }
            }
        } else if (requestType == APPEND_REQUEST) {
            if (access(&betterURIString[1], F_OK) != 0) {
                failHandler(connfd, errno, requestType, betterURIString, IDLength);
                memset(requestBuffer, 0, BUFFER_LENGTH);

                goto restartThread;
            }
        }

        if ((access(&betterURIString[1], W_OK) != 0) && (newFileStatus != FILE_CREATED)) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        sprintf(tempURIString, "%s-XXXXXX", &betterURIString[1]);

        tempFile = mkstemp(tempURIString);
        //unlink(tempURIString);
        //printf("FILE: %d\n", tempFile);
        openStatus = errno;
        if (tempFile == -1) {
            failHandler(connfd, errno, requestType, betterURIString, IDLength);
            memset(requestBuffer, 0, BUFFER_LENGTH);

            goto restartThread;
        }
        tempBody = strstr(requestBuffer, endofHeader);
        cursedPointer = tempBody + strlen("\r\n\r\n");
        rollingContent = contentLength;
        trueHeaderLength = cursedPointer - &requestBuffer[0];
        //printf"%d vs %d\n", trueHeaderLength, n);
        if (trueHeaderLength == n && contentLength > 0) {
            expectedContinue = CONTINUE_EXPECTED;
        }
        if (expectedContinue == CONTINUE_EXPECTED) {
        tempExpectedContinueLabel:
            memset(goodResponse, 0, BUFFER_LENGTH);
            checkpoint = tempExpectedContinue;
            POLL(thisPoll, thisConn, checkpoint);
            n = read(connfd, finalBuffer, BUFFER_LENGTH - 1);

            checkReadWrite(n, requestType);
            if (n < contentLength) {
                totalWritten += write(tempFile, finalBuffer, n);
                evaluateWrite(totalWritten, requestType);
            } else {
                totalWritten += write(tempFile, finalBuffer, contentLength);
                goto tempComplete;
            }
            readingStatus = KEEP_READING;
            goto expectedContinueStartTemp;
        } else {
            memset(goodResponse, 0, BUFFER_LENGTH);
        }

        if (tempBody) {
            if (n - trueHeaderLength > contentLength) {
                totalWritten += write(tempFile, cursedPointer, contentLength);
                //write(connfd, goodResponse, strlen(goodResponse));
                //memset(finalBuffer, 0, BUFFER_LENGTH);
                //memset(goodResponse, 0, BUFFER_LENGTH);
                goto tempComplete;
            } else {
                tempValue = write(tempFile, cursedPointer, n - trueHeaderLength);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                rollingContent = contentLength - tempValue;
                readingStatus = KEEP_READING;
            }
        }
        tempBody = NULL;
        cursedPointer = NULL;
    expectedContinueStartTemp:
        memset(requestBuffer, 0, BUFFER_LENGTH);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        int t;
        if (readingStatus == KEEP_READING) {
        tempKeepReadingLabel:
            memset(finalBuffer, 0, BUFFER_LENGTH);
            checkpoint = tempKeepReading;
            POLL(thisPoll, thisConn, checkpoint);
            t = read(connfd, finalBuffer, BUFFER_LENGTH - 1);

            //printf"Length of Q: %d\n", t);
            checkReadWrite(t, requestType);
            while (t > 0) {
                if (t > rollingContent) {
                    totalWritten += write(tempFile, finalBuffer, rollingContent);
                    evaluateWrite(totalWritten, requestType);
                    //checkReadWrite(rwStatus, requestType);
                    break;
                }
                tempValue = write(tempFile, finalBuffer, t);
                totalWritten += tempValue;
                evaluateWrite(totalWritten, requestType);
                //checkReadWrite(rwStatus, requestType);
                rollingContent = rollingContent - tempValue;

            tempLoopReadingLabel:
                memset(finalBuffer, 0, BUFFER_LENGTH);
                checkpoint = tempLoopReading;
                POLL(thisPoll, thisConn, checkpoint);
                t = read(connfd, finalBuffer, BUFFER_LENGTH - 1);

                TIMEOUT_CHECK_LOOP(t);
            }
        }
    tempComplete:
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
        //printf("%s\n", goodResponse);
        //write(connfd, goodResponse, strlen(goodResponse));
        //audit(requestType, betterURIString, status, IDLength);
        memset(finalBuffer, 0, BUFFER_LENGTH);
        memset(goodResponse, 0, BUFFER_LENGTH);
    }
    if (requestType == PUT_REQUEST || requestType == APPEND_REQUEST)
        goto finalRequest;
}

static void sigterm_handler(int sig) {
    if (sig == SIGTERM) {
        warnx("received SIGTERM");
        fclose(logfile);
        destroyAllThreads = 1;
        for (int i = 0; i < QUEUE_SIZE; i++) {
            close(connections[i].connfd);
        }
        for (int i = 0; i < superThreads; i++) {
            pthread_detach(thread[i]);
            pthread_join(thread[i], 0);
            //pthread_cancel(thread[i]);
            //pthread_kill(thread[i], SIGTERM);
            //pthread_join(thread[i], NULL);
        }
        free(thread);
        exit(EXIT_SUCCESS);
    }
}

static void sigint_handler(int sig) {
    if (sig == SIGINT) {
        warnx("received SIGINT");
        fclose(logfile);
        destroyAllThreads = 1;
        for (int i = 0; i < QUEUE_SIZE; i++) {
            close(connections[i].connfd);
        }
        for (int i = 0; i < superThreads; i++) {
            pthread_detach(thread[i]);
            pthread_join(thread[i], 0);
            //pthread_cancel(thread[i]);
            //pthread_kill(thread[i], SIGTERM);
            //pthread_join(thread[i], NULL);
        }
        free(thread);
        exit(EXIT_SUCCESS);
    }
}

static void usage(char *exec) {
    fprintf(stderr, "usage: %s [-t threads] [-l logfile] <port>\n", exec);
}

int main(int argc, char *argv[]) {
    table = hcreate(TABLE_SIZE);
    /*ENTRY item, itemTwo;
    ENTRY *lookItem;
    item.key = "test";
    item.data = (void *) 5;
    itemTwo.key = "sample";
    itemTwo.data = (void *) 10;
    hsearch(item, ENTER);
    hsearch(itemTwo, ENTER);*/
    //static pthread_mutex_t fileMTX[1024];
    for (int i = 0; i < ARBITRARY_THREAD_QUEUE; i++) {
        sem_init(&fileLock[i], 0, 1);
        sem_init(&readLock[i], 0, 1);
    }
    int opt = 0;
    pthread_mutex_lock(&queueMTX);
    int threads = DEFAULT_THREAD_COUNT;
    superThreads = threads;
    pthread_mutex_unlock(&queueMTX);
    logfile = stderr;

    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            pthread_mutex_lock(&queueMTX);
            threads = strtol(optarg, NULL, 10);
            superThreads = threads;
            pthread_mutex_unlock(&queueMTX);
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
    //signal(SIG_DIE_THREAD, sigdie_handler);

    int connectNum = 0;
    thread = (pthread_t *) malloc(sizeof(pthread_t) * superThreads);
    //threadfd threadConn[16];

    int listenfd = create_listen_socket(port);
    //LOG("port=%" PRIu16 ", threads=%d\n", port, threads);
    //int i = 0;
    for (int i = 0; i < threads; i++) {
        pthread_create(&thread[i], NULL, *handle_connection, 0);
    }
    //int super = 0;
    for (;;) {

        int connfd = accept(listenfd, NULL, NULL);
        //super++;
        //if (super % 100 == 0) {
        //    printf("%d\n",super);
        //}
        if (connfd < 0) {
            warn("accept error");
            continue;
        }
        pthread_mutex_lock(&queueMTX);
        while (numInQueue == QUEUE_SIZE) {
            pthread_cond_wait(&writeQueue, &queueMTX);
        }
        resetObject(connections[addQueue]);
        connections[addQueue].connfd = connfd;
        connections[addQueue].processingFinished = NEW_PROCESS;
        addQueue = (addQueue + 1) % QUEUE_SIZE;
        numInQueue++;
        pthread_mutex_unlock(&queueMTX);
        pthread_cond_signal(&readQueue);

        //i++;
        //close(connections[i]);
    }

    return EXIT_SUCCESS;
}
