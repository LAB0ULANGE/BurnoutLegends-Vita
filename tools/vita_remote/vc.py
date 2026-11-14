import socket, sys
def cmd(c, timeout=5):
    s = socket.create_connection(("192.168.1.21", 1338), timeout=timeout)
    s.sendall((c + "\n").encode())
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            out += d
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace")
print(cmd(" ".join(sys.argv[1:])))
