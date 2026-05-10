import socket
import struct

try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False


def get_ip(ifname="wlan0"):
    if not _HAS_FCNTL:
        return socket.gethostbyname(socket.gethostname())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )


def get_mac_address(ifname="wlan0"):
    try:
        with open(f"/sys/class/net/{ifname}/address", "r") as f:
            return f.read().strip()
    except Exception:
        return "unknown"