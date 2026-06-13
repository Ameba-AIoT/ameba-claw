# udp  —  require("udp")

Send:    sock = open(host, port) -> int;  send(sock, data) -> true;  close(sock)
Receive: sock = bind(port) -> int;
         data, ip, port = recv(sock [, maxlen [, timeout_ms]])  (nil on timeout)
         close(sock)

All socks are integer fds — safe to use inside timer code strings.
