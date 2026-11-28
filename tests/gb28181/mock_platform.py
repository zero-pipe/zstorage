#!/usr/bin/env python3
"""GB28181 本地模拟平台（UDP/TCP SIP + RTP/PS 收流）。

验证 ZCS 设备端：
  REGISTER(Digest) -> Catalog/DeviceInfo/DeviceStatus/RecordInfo ->
  INVITE(UDP / TCP 主被动) -> ACK -> 收 RTP/PS -> BYE

示例：
  python tests/gb28181/mock_platform.py --out out.ps
  python tests/gb28181/mock_platform.py --media-tcp listen --out tcp.ps
  python tests/gb28181/mock_platform.py --media-tcp connect --out tcp.ps
  python tests/gb28181/mock_platform.py --playback --out play.ps
"""
import argparse
import random
import re
import socket
import ssl
import sys
import time


# ── SIP ────────────────────────────────────────────────────────────────────

def parse_sip(data):
    text = data.decode("utf-8", "replace")
    head, _, body = text.partition("\r\n\r\n")
    if not body:
        head, _, body = text.partition("\n\n")
    lines = head.replace("\r\n", "\n").split("\n")
    start = lines[0] if lines else ""
    headers = {}
    last = None
    for line in lines[1:]:
        if not line:
            continue
        if line[0] in " \t" and last:
            headers[last] += " " + line.strip()
            continue
        if ":" in line:
            k, v = line.split(":", 1)
            last = k.strip().lower()
            headers[last] = v.strip()
    return start, headers, body


def cseq_num(headers):
    m = re.match(r"\s*(\d+)", headers.get("cseq", "0 X"))
    return int(m.group(1)) if m else 0


def cseq_method(headers):
    parts = headers.get("cseq", "0 X").split()
    return parts[1] if len(parts) > 1 else ""


def rand_hex(n=16):
    return "".join(random.choice("0123456789abcdef") for _ in range(n))


def build_request(method, uri, from_user, to_user, domain, call_id, cseq, local_addr,
                  subject=None, content_type=None, body=b""):
    host, port = local_addr
    lines = [
        f"{method} {uri} SIP/2.0",
        f"Via: SIP/2.0/UDP {host}:{port};rport;branch=z9hG4bK{rand_hex()}",
        f"From: <sip:{from_user}@{domain}>;tag={random.getrandbits(32):08x}",
        f"To: <sip:{to_user}@{domain}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} {method}",
        "Max-Forwards: 70",
        "User-Agent: gb28181-mock",
    ]
    if subject:
        lines.append(f"Subject: {subject}")
    if body:
        lines.append(f"Content-Type: {content_type}")
    lines.append(f"Content-Length: {len(body)}")
    lines.append("")
    lines.append("")
    return ("\r\n".join(lines)).encode() + body


def build_response(headers, code, reason, extra=(), body=b"", content_type=None):
    lines = [f"SIP/2.0 {code} {reason}"]
    for key, name in (("via", "Via"), ("from", "From"), ("to", "To"),
                      ("call-id", "Call-ID"), ("cseq", "CSeq")):
        if key in headers:
            lines.append(f"{name}: {headers[key]}")
    for line in extra:
        lines.append(line)
    if content_type:
        lines.append(f"Content-Type: {content_type}")
    lines.append(f"Content-Length: {len(body)}")
    lines.append("")
    lines.append("")
    return ("\r\n".join(lines)).encode() + body


def build_ack(invite_headers, resp_headers, local_addr):
    contact = resp_headers.get("contact", "")
    m = re.search(r"<([^>]+)>", contact)
    uri = m.group(1) if m else invite_headers.get("contact", "")
    host, port = local_addr
    lines = [
        f"ACK {uri} SIP/2.0",
        f"Via: SIP/2.0/UDP {host}:{port};rport;branch=z9hG4bK{rand_hex()}",
        f"From: {invite_headers.get('from', '')}",
        f"To: {resp_headers.get('to', invite_headers.get('to', ''))}",
        f"Call-ID: {invite_headers.get('call-id', '')}",
        f"CSeq: {cseq_num(invite_headers)} ACK",
        "Max-Forwards: 70",
        "Content-Length: 0",
        "",
        "",
    ]
    return "\r\n".join(lines).encode()


def build_bye(invite_headers, resp_headers, local_addr, uri):
    host, port = local_addr
    lines = [
        f"BYE {uri} SIP/2.0",
        f"Via: SIP/2.0/UDP {host}:{port};rport;branch=z9hG4bK{rand_hex()}",
        f"From: {invite_headers.get('from', '')}",
        f"To: {resp_headers.get('to', invite_headers.get('to', ''))}",
        f"Call-ID: {invite_headers.get('call-id', '')}",
        f"CSeq: {cseq_num(invite_headers) + 1} BYE",
        "Max-Forwards: 70",
        "Content-Length: 0",
        "",
        "",
    ]
    return "\r\n".join(lines).encode()


def answer_media(body):
    host, port = None, None
    for line in body.replace("\r\n", "\n").split("\n"):
        if line.startswith("c=IN IP4 "):
            host = line[9:].strip()
        if line.startswith("m=video "):
            try:
                port = int(line.split()[1])
            except (IndexError, ValueError):
                pass
    return host, port


# ── 模拟平台 ───────────────────────────────────────────────────────────────

class MockPlatform:
    def __init__(self, args):
        self.args = args
        self.sip_connect = bool(args.sip_connect or args.sip_tls_connect)
        self.tls_connect = bool(args.sip_tls_connect)
        self.tcp_sip = bool(args.sip_tcp or args.sip_tls or self.sip_connect)
        self.tls_sip = bool(args.sip_tls)
        if self.sip_connect:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setblocking(False)
            self.conn = None
        elif self.tcp_sip:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.bind(("0.0.0.0", args.sip_port))
            self.sock.listen(1)
            self.sock.setblocking(False)
            self.conn = None
        else:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(("0.0.0.0", args.sip_port))
            self.sock.setblocking(False)
            self.conn = None
        self.device_addr = None
        self.cseq = 1
        self.rtp = bytearray()
        self.tcp_buf = b""
        self.sip_rx = b""
        self.sip_queue = []
        self.invite_headers = None

    def send(self, data, addr=None):
        if self.tcp_sip:
            if self.conn is not None:
                self.conn.sendall(data)
            return
        self.sock.sendto(data, addr or self.device_addr)

    def _tcp_extract(self):
        sep = self.sip_rx.find(b"\r\n\r\n")
        sep_len = 4
        if sep < 0:
            sep = self.sip_rx.find(b"\n\n")
            sep_len = 2
        if sep < 0:
            return False
        header = self.sip_rx[:sep].decode("latin1", "replace")
        clen = 0
        for line in header.replace("\r\n", "\n").split("\n"):
            if ":" in line:
                k, v = line.split(":", 1)
                if k.strip().lower() in ("content-length", "l"):
                    try:
                        clen = int(v.strip())
                    except ValueError:
                        clen = 0
        total = sep + sep_len + clen
        if len(self.sip_rx) < total:
            return False
        self.sip_queue.append(self.sip_rx[:total])
        self.sip_rx = self.sip_rx[total:]
        return True

    def recv_sip(self):
        if self.sip_connect and self.conn is None:
            raw = None
            try:
                raw = socket.create_connection((self.args.device_ip, self.args.device_port),
                                               timeout=2.0)
                if self.tls_connect:
                    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    ctx.check_hostname = False
                    ctx.verify_mode = ssl.CERT_NONE
                    try:
                        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
                        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
                        ctx.set_ciphers("ECDHE-RSA-AES128-GCM-SHA256:"
                                        "ECDHE-RSA-AES256-GCM-SHA384:AES128-SHA")
                    except (AttributeError, ssl.SSLError):
                        pass
                    raw = ctx.wrap_socket(raw, server_hostname=self.args.device_ip)
                    print(f"[sip] TLS connected to device {self.args.device_ip}:"
                          f"{self.args.device_port}")
                else:
                    print(f"[sip] TCP connected to device {self.args.device_ip}:"
                          f"{self.args.device_port}")
                raw.setblocking(False)
                self.conn = raw
                self.device_addr = (self.args.device_ip, self.args.device_port)
            except (socket.timeout, ssl.SSLError, OSError) as exc:
                if raw is not None:
                    try:
                        raw.close()
                    except Exception:
                        pass
                if self.tls_connect:
                    print(f"[sip] TLS connect failed: {exc}")
                return None, None
        if self.tcp_sip:
            if self.conn is None:
                raw = None
                try:
                    self.sock.settimeout(0.5 if self.tls_sip else 0.0)
                    raw, peer = self.sock.accept()
                    self.sock.setblocking(False)
                    if self.tls_sip:
                        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                        ctx.load_cert_chain(self.args.tls_cert, self.args.tls_key)
                        raw.settimeout(3.0)
                        print(f"[sip] TLS handshake with {peer[0]}:{peer[1]} ...")
                        self.conn = ctx.wrap_socket(raw, server_side=True)
                        print(f"[sip] TLS accepted {peer[0]}:{peer[1]}")
                    else:
                        self.conn = raw
                        print(f"[sip] TCP accepted {peer[0]}:{peer[1]}")
                    self.conn.setblocking(False)
                    self.device_addr = peer
                except (BlockingIOError, socket.timeout, ssl.SSLError, OSError) as exc:
                    if raw is not None:
                        try:
                            raw.close()
                        except Exception:
                            pass
                    if self.tls_sip:
                        print(f"[sip] TLS accept/handshake failed: {exc}")
                    return None, None
            try:
                data = self.conn.recv(65535)
                if data:
                    self.sip_rx += data
                elif data == b"":
                    return None, None
            except BlockingIOError:
                pass
            except OSError:
                return None, None
            while self._tcp_extract():
                pass
            if self.sip_queue:
                msg = self.sip_queue.pop(0)
                peer = self.device_addr or ("127.0.0.1", 0)
                return msg, peer
            return None, None
        try:
            return self.sock.recvfrom(65535)
        except BlockingIOError:
            return None, None

    # 媒体传输
    def media_create(self):
        mode = self.args.media_tcp
        if mode == "listen":
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", self.args.media_port))
            s.listen(1)
            s.setblocking(False)
            return s, "listen"
        if mode == "connect":
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setblocking(False)
            return s, "connect"
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
        s.bind(("0.0.0.0", self.args.media_port))
        s.setblocking(False)
        return s, "udp"

    def media_drain(self, media, kind):
        if kind == "udp":
            while True:
                try:
                    pkt, _ = media.recvfrom(2048)
                except BlockingIOError:
                    break
                if len(pkt) > 12:
                    self.rtp.extend(pkt[12:])
            return
        try:
            data = media.recv(65536)
        except BlockingIOError:
            return
        except OSError:
            return
        if not data:
            return
        self.tcp_buf += data
        while len(self.tcp_buf) >= 2:
            n = (self.tcp_buf[0] << 8) | self.tcp_buf[1]
            if len(self.tcp_buf) < 2 + n:
                break
            pkt = self.tcp_buf[2:2 + n]
            self.tcp_buf = self.tcp_buf[2 + n:]
            if len(pkt) > 12:
                self.rtp.extend(pkt[12:])

    # 请求
    def send_queries(self):
        cmds = [("Catalog", ""), ("DeviceInfo", ""), ("DeviceStatus", "")]
        if self.args.playback:
            cmds.append(("RecordInfo",
                         f"<StartTime>{self.args.query_start}</StartTime>"
                         f"<EndTime>{self.args.query_end}</EndTime>"
                         "<Secrecy>0</Secrecy><Type>all</Type>"))
        for cmd, extra in cmds:
            self.cseq += 1
            body = (f"<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Query>\r\n"
                    f"<CmdType>{cmd}</CmdType>\r\n<SN>{self.cseq}</SN>\r\n"
                    f"<DeviceID>{self.args.channel_id}</DeviceID>\r\n{extra}</Query>\r\n").encode()
            req = build_request(
                "MESSAGE", f"sip:{self.args.device_id}@{self.args.platform_domain}",
                self.args.platform_id, self.args.device_id, self.args.platform_domain,
                f"{random.getrandbits(64):016x}@{self.args.platform_id}", self.cseq,
                ("127.0.0.1", self.args.sip_port),
                content_type="Application/MANSCDP+xml", body=body)
            self.send(req)
            print(f"[sip] -> {cmd} query")
            time.sleep(0.3 if cmd == "RecordInfo" else 0.2)

    def send_invite(self):
        ssrc = self.args.ssrc
        proto = "RTP/AVP"
        setup = ""
        if self.args.media_tcp == "listen":
            proto = "TCP/RTP/AVP"
            setup = "a=setup:passive\r\n"
        elif self.args.media_tcp == "connect":
            proto = "TCP/RTP/AVP"
            setup = "a=setup:active\r\n"
        if self.args.playback:
            ts = int(time.mktime(time.strptime(self.args.play_start, "%Y-%m-%dT%H:%M:%S")))
            te = int(time.mktime(time.strptime(self.args.play_end, "%Y-%m-%dT%H:%M:%S")))
            sdp = (
                "v=0\r\n"
                f"o={self.args.platform_id} 0 0 IN IP4 127.0.0.1\r\n"
                "s=Playback\r\n"
                f"u={self.args.channel_id}:0\r\n"
                "c=IN IP4 127.0.0.1\r\n"
                f"t={ts} {te}\r\n"
                f"m=video {self.args.media_port} {proto} 96\r\n"
                "a=recvonly\r\n"
                "a=rtpmap:96 PS/90000\r\n"
                f"{setup}y={ssrc}\r\n"
            ).encode()
        else:
            sdp = (
                "v=0\r\n"
                f"o={self.args.platform_id} 0 0 IN IP4 127.0.0.1\r\n"
                "s=Play\r\n"
                "c=IN IP4 127.0.0.1\r\n"
                "t=0 0\r\n"
                f"m=video {self.args.media_port} {proto} 96\r\n"
                "a=recvonly\r\n"
                "a=rtpmap:96 PS/90000\r\n"
                f"{setup}y={ssrc}\r\n"
            ).encode()
        self.cseq += 1
        subject = f"{self.args.channel_id}:{ssrc},{self.args.platform_id}:0"
        req = build_request(
            "INVITE", f"sip:{self.args.channel_id}@127.0.0.1",
            self.args.platform_id, self.args.channel_id, self.args.platform_domain,
            f"{random.getrandbits(64):016x}@{self.args.platform_id}", self.cseq,
            ("127.0.0.1", self.args.sip_port),
            subject=subject, content_type="APPLICATION/SDP", body=sdp)
        self.send(req)
        mode = "udp" if proto == "RTP/AVP" else self.args.media_tcp
        print(f"[sip] -> INVITE channel={self.args.channel_id} ssrc={ssrc} media={mode}")
        return parse_sip(req)[1]

    # 主循环
    def serve(self):
        media, kind = self.media_create()
        state = "wait_register"
        got_200 = False
        media_ready = kind == "udp"
        rtp_started = 0.0
        deadline = time.time() + self.args.seconds

        while time.time() < deadline:
            data, addr = self.recv_sip()
            if data:
                start, headers, body = parse_sip(data)
                print(f"[sip] <- {start} from {addr[0]}:{addr[1]} len={len(body)}")
                if start.startswith("SIP/2.0"):
                    code = int(start.split()[1])
                    if code == 200 and cseq_method(headers) == "REGISTER":
                        if state == "wait_register":
                            state = "registered"
                            self.device_addr = addr
                            m = re.search(r"<(sip:[^>]+)>", headers.get("contact", ""))
                            print(f"[sip] registered from {addr[0]}:{addr[1]} "
                                  f"contact={m.group(1) if m else '?'}")
                            if self.args.start_delay > 0:
                                time.sleep(self.args.start_delay)
                            self.send_queries()
                            self.invite_headers = self.send_invite()
                    elif code == 200 and cseq_method(headers) == "INVITE":
                        got_200 = True
                        rtp_started = time.time()
                        print(f"[sip] INVITE 200 OK sdp={body[:100]!r}")
                        self.send(build_ack(self.invite_headers, headers,
                                            ("127.0.0.1", self.args.sip_port)), addr)
                        if kind == "connect":
                            host, port = answer_media(body)
                            try:
                                media.settimeout(3.0)
                                media.connect((host, port))
                                media.setblocking(False)
                                media_ready = True
                                print(f"[media] TCP connected to device {host}:{port}")
                            except OSError as exc:
                                print(f"[media] TCP connect {host}:{port} failed: {exc}")
                        self.resp_headers = headers
                        self.device_contact = headers.get("contact", "")
                    elif code == 200 and cseq_method(headers) == "BYE":
                        print("[sip] BYE 200 OK")
                else:
                    method = start.split()[0]
                    if method == "REGISTER":
                        if "authorization" not in headers:
                            chal = ("WWW-Authenticate: Digest "
                                    f"realm=\"{self.args.platform_domain}\", "
                                    "nonce=\"abcdef0123456789\", algorithm=MD5")
                            self.send(build_response(headers, 401, "Unauthorized",
                                                     extra=(chal,)), addr)
                            print("[sip] -> 401 challenge")
                        else:
                            self.send(build_response(headers, 200, "OK",
                                                     extra=("Expires: 3600",)), addr)
                            if state == "wait_register":
                                state = "registered"
                                self.device_addr = addr
                                m = re.search(r"<(sip:[^>]+)>", headers.get("contact", ""))
                                print(f"[sip] registered from {addr[0]}:{addr[1]} "
                                      f"contact={m.group(1) if m else '?'}")
                                if self.args.start_delay > 0:
                                    time.sleep(self.args.start_delay)
                                self.send_queries()
                                self.invite_headers = self.send_invite()
                    elif method == "MESSAGE":
                        self.send(build_response(headers, 200, "OK"), addr)
                        if "<CmdType>Catalog</CmdType>" in body and "<Response>" in body:
                            print(f"[manscdp] catalog items={body.count('<Item>')}")
                        elif "<CmdType>DeviceInfo</CmdType>" in body:
                            print("[manscdp] device info response")
                        elif "<CmdType>DeviceStatus</CmdType>" in body:
                            print("[manscdp] device status response")
                        elif "<CmdType>RecordInfo</CmdType>" in body:
                            print(f"[manscdp] record items={body.count('<Item>')}")
                        elif "<CmdType>Alarm</CmdType>" in body:
                            print(f"[manscdp] alarm notify: {body[:200]!r}")
                    else:
                        self.send(build_response(headers, 200, "OK"), addr)

            if kind == "listen" and not media_ready:
                try:
                    media, _ = media.accept()
                    media.setblocking(False)
                    media_ready = True
                    print("[media] TCP accepted device connection")
                except BlockingIOError:
                    pass

            if got_200 and media_ready:
                self.media_drain(media, kind)

            if got_200 and len(self.rtp) > 0 and \
                    time.time() - rtp_started > self.args.collect:
                print(f"[rtp] collected {len(self.rtp)} bytes, sending BYE")
                contact = getattr(self, "device_contact", "")
                m = re.search(r"<([^>]+)>", contact)
                uri = m.group(1) if m else f"sip:{self.args.channel_id}@127.0.0.1"
                self.send(build_bye(self.invite_headers, self.resp_headers,
                                    ("127.0.0.1", self.args.sip_port), uri))
                break

        media.close()
        if self.conn is not None:
            self.conn.close()
        self.sock.close()
        with open(self.args.out, "wb") as f:
            f.write(self.rtp)
        print(f"[ps] wrote {self.args.out} ({len(self.rtp)} bytes)")
        return self.rtp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sip-port", type=int, default=15060)
    ap.add_argument("--media-port", type=int, default=16000)
    ap.add_argument("--sip-tcp", action="store_true", help="平台作为 SIP TCP 服务端（设备主动连）")
    ap.add_argument("--sip-tls", action="store_true", help="平台作为 SIP TLS 服务端（设备主动连）")
    ap.add_argument("--sip-connect", action="store_true", help="平台主动连设备 SIP TCP 端口")
    ap.add_argument("--sip-tls-connect", action="store_true", help="平台主动连设备 SIP TLS 端口")
    ap.add_argument("--device-ip", default="127.0.0.1")
    ap.add_argument("--device-port", type=int, default=15061)
    ap.add_argument("--tls-cert", default="tls_cert.pem")
    ap.add_argument("--tls-key", default="tls_key.pem")
    ap.add_argument("--start-delay", type=float, default=0.0,
                    help="注册成功后、发查询/INVITE 前的等待秒数（等推流上线）")
    ap.add_argument("--media-tcp", choices=("none", "listen", "connect"), default="none",
                    help="listen=平台监听(设备主动连)；connect=平台主动连设备")
    ap.add_argument("--device-id", default="34020000001320000001")
    ap.add_argument("--platform-id", default="34020000002000000001")
    ap.add_argument("--platform-domain", default="3402000000")
    ap.add_argument("--channel-id", default="34020000001320000001")
    ap.add_argument("--ssrc", default="0100000001")
    ap.add_argument("--out", default="gb28181_out.ps")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--collect", type=float, default=4.0)
    ap.add_argument("--playback", action="store_true")
    ap.add_argument("--query-start", default="2026-11-09T10:00:00")
    ap.add_argument("--query-end", default="2026-11-09T10:00:10")
    ap.add_argument("--play-start", default="2026-11-09T10:00:02")
    ap.add_argument("--play-end", default="2026-11-09T10:00:08")
    args = ap.parse_args()
    if args.media_tcp == "none":
        args.media_tcp = None

    plat = MockPlatform(args)
    payload = plat.serve()
    if len(payload) < 1000:
        print(f"[fail] RTP/PS too small: {len(payload)}", file=sys.stderr)
        return 2
    if not (payload[0] == 0 and payload[1] == 0 and payload[2] == 1 and payload[3] == 0xBA):
        print("[fail] not a PS stream", file=sys.stderr)
        return 3
    if b"\x00\x00\x01\xbc" not in payload:
        print("[fail] PS missing PSM", file=sys.stderr)
        return 4
    print("[ok] PS stream received")
    return 0


if __name__ == "__main__":
    sys.exit(main())
