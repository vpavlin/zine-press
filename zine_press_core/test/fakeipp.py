#!/usr/bin/env python3
# A fake CUPS queue for Zine Press tests: serves a real PPD, answers IPP, logs every request.
# Put the queue's PPD next to it as DCPT230.ppd (a GET of http://HOST:631/printers/NAME.ppd; not in git:
# it is Brother's file), run it, then: ipp_print_test FILE.pdf <this folder>.
# Queue length comes from the file QUEUE (a number). Nothing is printed.
import json, os, struct, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "requests.jsonl")
STRING_TAGS = {0x41, 0x42, 0x44, 0x45, 0x47, 0x48, 0x49}
job_seq = [100]

def parse(body):
    ver, op, rid = struct.unpack(">HHI", body[:8])
    i, groups, cur, name = 8, [], None, None
    while i < len(body):
        tag = body[i]; i += 1
        if tag == 0x03: break
        if tag < 0x10:
            cur = {"group": tag, "attrs": {}}; groups.append(cur); continue
        nl = struct.unpack(">H", body[i:i+2])[0]; i += 2
        n = body[i:i+nl].decode(); i += nl
        vl = struct.unpack(">H", body[i:i+2])[0]; i += 2
        raw = body[i:i+vl]; i += vl
        if tag in (0x21, 0x23) and vl == 4: v = struct.unpack(">i", raw)[0]
        elif tag == 0x22: v = bool(raw[0])
        elif tag == 0x33: v = list(struct.unpack(">ii", raw))
        elif tag in STRING_TAGS: v = raw.decode(errors="replace")
        else: v = raw.hex()
        if nl: name = n; cur["attrs"].setdefault(name, []).append(v)
        else: cur["attrs"][name].append(v)
    return op, rid, groups, body[i:]

def attr(tag, name, values):
    out = b""
    for k, v in enumerate(values if isinstance(values, list) else [values]):
        nm = name.encode() if k == 0 else b""
        if tag in (0x21, 0x23): val = struct.pack(">i", v)
        elif tag == 0x22: val = bytes([1 if v else 0])
        else: val = str(v).encode()
        out += bytes([tag]) + struct.pack(">H", len(nm)) + nm + struct.pack(">H", len(val)) + val
    return out

def reply(rid, status, groups):
    b = struct.pack(">HHI", 0x0101, status, rid)
    b += b"\x01" + attr(0x47, "attributes-charset", "utf-8") + attr(0x48, "attributes-natural-language", "en")
    for gtag, attrs in groups:
        b += bytes([gtag]) + b"".join(attrs)
    return b + b"\x03"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.endswith(".ppd"):
            d = open(os.path.join(HERE, "DCPT230.ppd"), "rb").read()
            self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(d))); self.end_headers(); self.wfile.write(d)
        else:
            self.send_response(404); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        op, rid, groups, doc = parse(body)
        rec = {"op": hex(op), "groups": groups, "docBytes": len(doc)}
        with open(LOG, "a") as f: f.write(json.dumps(rec) + "\n")
        queue = int(open(os.path.join(HERE, "QUEUE")).read().strip() or 0) if os.path.exists(os.path.join(HERE, "QUEUE")) else 0
        uri = "ipp://127.0.0.1:8632/printers/DCPT230"
        if op == 0x000B:
            p = [attr(0x42, "printer-name", "DCPT230"), attr(0x41, "printer-info", "Fake DCP-T230"),
                 attr(0x41, "printer-make-and-model", "Brother DCP-T230 (fake)"), attr(0x23, "printer-state", 3),
                 attr(0x44, "printer-state-reasons", "none"), attr(0x22, "printer-is-accepting-jobs", True),
                 attr(0x49, "document-format-supported", ["application/pdf", "application/octet-stream"]),
                 attr(0x44, "sides-supported", "one-sided"), attr(0x21, "queued-job-count", queue)]
            out = reply(rid, 0, [(0x04, p)])
        elif op == 0x0002:
            job_seq[0] += 1
            out = reply(rid, 0, [(0x02, [attr(0x21, "job-id", job_seq[0]), attr(0x45, "job-uri", uri + "/" + str(job_seq[0])), attr(0x23, "job-state", 3)])])
        elif op == 0x0009:
            out = reply(rid, 0, [(0x02, [attr(0x23, "job-state", 9), attr(0x21, "job-impressions-completed", 1), attr(0x21, "job-media-sheets-completed", 1)])])
        elif op == 0x000A:
            out = reply(rid, 0, [(0x02, [attr(0x21, "job-id", 1 + k), attr(0x23, "job-state", 3)]) for k in range(queue)])
        else:
            out = reply(rid, 0, [])
        self.send_response(200); self.send_header("Content-Type", "application/ipp"); self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)

ThreadingHTTPServer(("127.0.0.1", 8632), H).serve_forever()
