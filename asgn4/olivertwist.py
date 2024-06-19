#!/usr/bin/env python3

from enum import Enum
import argparse, logging, os, select, toml, time, sys, shutil, re, socket, sys

class Req_State(Enum):
    CONNECTED = 1
    SENT_LINE = 2
    SENT_HEADERS = 3
    SENDING_BODY = 4
    SENT = 5
    RECEIVED = 6

class Req_Method(Enum):
    GET = 1
    PUT = 2
    APPEND = 3

    def to_string(self):
        if self == Req_Method.GET:
            return "GET"
        elif self == Req_Method.PUT:
            return "PUT"
        else:
            return "APPEND"

class request():
    ''' Reprsents a new connection with a client '''

    response_regex = re.compile(b"HTTP/1.1 ([^ ]*) (.*)\r\n")

    def __init__(self, hostname, port, method, uri, rid, inbody, outfile):
        '''
        Creates a new client request:
        hostname: the name of the host to connect to
        port: the port to connect with
        uri: the URI to refernece in the request
        method: The method to use, of time Req_Method
        rid: The Request ID to use
        in_body: the data to send (must be encoded as UTF-8, I hope?)
        out_body: where to place the message body part of the output
        '''

        request_line = f"{method.to_string()} /{uri} HTTP/1.1\r\n"
        headers = [f"Request-Id: {rid}\r\n"]

        if len(inbody) > 0:
            headers.append(f"Content-Length: {len(inbody)}\r\n")

        self.bytez = [bytearray(request_line.encode('UTF-8'))]
        headerz = bytearray()
        for h in headers:
            headerz.extend(h.encode('UTF-8'))
        headerz.extend(b"\r\n")
        self.bytez.append(headerz)
        self.bytez.append(inbody)

        assert (len(self.bytez) == 3)

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((socket.gethostbyname(hostname), port))
        self.state = Req_State.CONNECTED

        self.rid = rid
        self.outfile = outfile
        self.sent = 0
        self.state = Req_State.CONNECTED
        self.received = bytearray()

        self.partial_flag = False


    def send_line(self):
        assert (self.state == Req_State.CONNECTED)
        self.sock.sendall(self.bytez[0])
        self.state = Req_State.SENT_LINE

    def send_headers(self):
        assert (self.state == Req_State.SENT_LINE)
        self.sock.sendall(self.bytez[1])
        self.state = Req_State.SENT_HEADERS

    def send_body(self, size):
        assert (self.state == Req_State.SENT_HEADERS or\
                self.state == Req_State.SENDING_BODY)

        start = self.sent
        end = self.sent + size

        if size == -1 or len(self.bytez[-1]) - self.sent < size:
            end = len(self.bytez[-1])

        self.sock.sendall(self.bytez[-1][start : end])
        self.sent = end

        if len(self.bytez[-1]) == self.sent:
            self.state = Req_State.SENT
            self.sock.shutdown(socket.SHUT_WR)
            return self.sock
        else:
            self.state = Req_State.SENDING_BODY

        return None

    def recv_data(self, size = 4096):
        if self.state != Req_State.SENT:
            print(f"what are you!? {self.state}", file=sys.stderr)
        assert (self.state == Req_State.SENT)

        data = self.sock.recv(size)

        if data:
            self.received.extend(data)
        else:
            self.state = Req_State.RECEIVED
            self.sock.close()
            return True
        return False

    def wait(self):
        assert (self.state == Req_State.SENT or\
                self.state == Req_State.RECEIVED)

        match = re.match(request.response_regex, self.received)
        code = -1
        if match:
            code = int(match.group(1))
        bodyindex = self.received.find(b"\r\n\r\n") + 4
        if self.outfile != None:
            open(self.outfile, "wb+").write(self.received[bodyindex:])

        return code

def argparser():
    parser = argparse.ArgumentParser(description="Issues a batch of requests.")
    parser.add_argument(
        "-o", "--hostname", type=str, default="localhost",
        metavar="localhost", help="Hostname to connect to."
    )
    parser.add_argument(
        "-p", "--port", type=int,
        metavar="port", help="Port to connect on."
    )
    parser.add_argument(
        "-d", "--dir", type=str, default="",
        metavar="dir", help="Where to place the message bodies."
    )

    parser.add_argument(
        "-m", "--maxreqs", type=int, default=10,
        metavar="maxreqs", help="Max number of outstanding requests."
    )

    parser.add_argument(
        "requests", type=str, metavar="requests",
        help="file containing a batch of tests."
    )
    return parser.parse_args()


def poll_once(poller, readers):
    descs = poller.poll(0)
    for r, _ in descs:
        if readers[r].recv_data():
            log("RECEIVED", readers[r].rid)
            del readers[r]
            poller.unregister(r)

def readem(poller, readers, max_readers):
    poll_once(poller, readers)
    while len(readers) >= max_readers:
        poll_once(poller, readers)

log_events=[]
def log(event, id, *argv):
    stamp = time.time()
    fields = [stamp, event, id]
    if argv:
        fields.extend(argv)
    log_events.append(fields)

def flush_log():
    for l in log_events:
        print(", ".join([str(f) for f in l]))

def load(event):
    if "infile" in event and "outfile" in event:
        shutil.copyfile(event["infile"], event["outfile"])

def unload(event):
    if "file" in event:
        os.remove(event["file"])

def create(rid, event, hostname, port, outdir):
    uri = event["uri"]
    outfile = f"{rid}-out"
    if len(outdir) > 0:
        outfile = f"{outdir}/{outfile}"

    inbody = b""
    if "infile" in event:
        inbody = open(event["infile"], "r").read().encode("UTF-8")

    method = Req_Method.GET
    if "method" in event and event["method"] == "PUT":
        method = Req_Method.PUT
    elif "method" in event and event["method"] == "APPEND":
        method = Req_Method.APPEND

    return request(hostname, port, method, uri, rid, inbody, outfile)

def main():
    requests = {}
    receivers = {}
    args = argparser()
    poller = select.epoll()

    basename = f"{args.hostname}:{args.port}"
    outdir = ""
    if len(args.dir) > 0:
        outdir = args.dir
        try:
            os.mkdir(outdir)
        except:
            pass

    for event in toml.load(args.requests)["events"]:
        readem(poller, receivers, args.maxreqs)

        rid = -1
        if "id" in event:
            rid = event["id"]

        if event["type"] == "LOAD":
            load(event)
            log("LOAD", f'{event["infile"]}, {event["outfile"]}')

        elif event["type"] == "UNLOAD":
            unload(event)
            log("UNLOAD", f'{event["file"]}')

        elif event["type"] == "SLEEP":
            seconds = 4
            if "seconds" in event:
                seconds = event["seconds"]
            time.sleep(seconds)
            log("SLEEP", seconds)

        elif event["type"] == "CREATE":
            request = create(rid, event, args.hostname, args.port, outdir)
            requests[rid] = request
            log("CONNECT", rid)

        elif event["type"] == "SEND_LINE":
            requests[rid].send_line()
            log("SEND_LINE", rid)

        elif event["type"] == "SEND_HEADERS":
            requests[rid].send_headers()
            log("SEND_HEADERS", rid)

        elif event["type"] == "SEND_BODY":
            size = -1
            if "size" in event:
                size = event["size"]

            sock = requests[rid].send_body(size)
            if sock != None:
                poller.register(sock.fileno(), select.EPOLLIN)
                receivers[sock.fileno()] = requests[rid]
                log("SENT_BODY", rid)

        elif event["type"] == "SEND_ALL":
            requests[rid].send_line()
            requests[rid].send_headers()
            sock = requests[rid].send_body(-1)
            poller.register(sock.fileno(), select.EPOLLIN)
            receivers[sock.fileno()] = requests[rid]
            log("SEND_ALL", rid)

        elif event["type"] == "RECV_PARTIAL":
            requests[rid].partial_flag = True
            requests[rid].state = Req_State.SENT
            requests[rid].recv_data(event["size"])
            requests[rid].sock.shutdown(socket.SHUT_WR)

        elif event["type"] == "WAIT":
            req = requests[rid]
            fd = req.sock.fileno()
            if fd > 0:
                if not requests[rid].partial_flag:
                    poller.unregister(fd)
                    del receivers[fd]

                while not req.recv_data():
                    pass
                log("RECEIVED", rid)

            code = req.wait()
            log("WAIT", rid, code)
            del requests[rid]

        else:
            print(f"That event, Oliver, is an imposter: {event}")

    flush_log()

if __name__ == "__main__":
    main()
