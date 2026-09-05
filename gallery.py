#!/usr/bin/env python3
import os, re, json, mimetypes, posixpath, urllib.parse, time, struct, zlib, functools
import http.server, socketserver

PORT = 8080
BIND = "0.0.0.0"

ROOT = os.path.dirname(os.path.abspath(__file__))
DIRS = {
    "images": os.path.join(ROOT, "images"),
    "videos": os.path.join(ROOT, "videos"),
}

IMG_EXT = {".png",".jpg",".jpeg",".gif",".webp",".bmp",".tif",".tiff",".avif"}
VID_EXT = {".mp4",".mov",".webm",".m4v",".mkv",".avi"}
SCAN_EXT = {".ppm",".png",".jpg",".jpeg"}
SKIP_DIRS = {"node_modules"}

mimetypes.add_type("video/quicktime", ".mov")
mimetypes.add_type("video/x-matroska", ".mkv")
mimetypes.add_type("video/mp4", ".m4v")
mimetypes.add_type("image/avif", ".avif")
mimetypes.add_type("image/png", ".png")


# ---- PPM -> PNG, pure stdlib (no Pillow) ----

def _png_chunk(typ, payload):
    c = typ + payload
    return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

def _encode_png(w, h, rgb):
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    raw = bytearray()
    ext = raw.extend
    for y in range(h):
        raw.append(0)
        ext(rgb[y*stride:(y+1)*stride])
    idat = zlib.compress(bytes(raw), 6)
    return sig + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"IDAT", idat) + _png_chunk(b"IEND", b"")

def _parse_ppm(data):
    # locate the P6 magic; tolerate log text accidentally prepended before it
    i = data.find(b"P6")
    if i < 0 or i > 8192:
        raise ValueError("no P6 magic")
    p = i + 2
    n = len(data)
    def next_int():
        nonlocal p
        while p < n:
            c = data[p]
            if c in (0x20, 0x09, 0x0d, 0x0a, 0x0b, 0x0c):
                p += 1; continue
            if c == 0x23:
                while p < n and data[p] != 0x0a:
                    p += 1
                continue
            break
        tok = []
        while p < n:
            c = data[p]
            if c in (0x20, 0x09, 0x0d, 0x0a, 0x0b, 0x0c) or c == 0x23:
                break
            tok.append(c); p += 1
        if not tok:
            raise ValueError("missing header number")
        return int(bytes(tok))
    w = next_int(); h = next_int(); mv = next_int()
    if not (1 <= w <= 65536 and 1 <= h <= 65536):
        raise ValueError("bad dimensions")
    if mv > 255:
        raise ValueError("maxval > 255 (16-bit unsupported)")
    p += 1  # exactly one whitespace byte separates header from raster
    need = w * h * 3
    if n - p < need:
        raise ValueError("truncated raster")
    rgb = data[p:p+need]
    if mv != 255:
        rgb = bytes((b * 255) // mv for b in rgb)
    return w, h, bytes(rgb)

@functools.lru_cache(maxsize=128)
def _ppm_png(path, mtime):
    with open(path, "rb") as f:
        data = f.read()
    w, h, rgb = _parse_ppm(data)
    return _encode_png(w, h, rgb)

_ERROR_PNG = _encode_png(320, 200, bytes((120, 40, 40)) * (320 * 200))


PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ray output</title><style>
*{box-sizing:border-box}
body{margin:0;background:#0d0d0f;color:#c8c8cc;
 font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
header{position:sticky;top:0;z-index:5;height:48px;background:#0d0d0fee;
 backdrop-filter:blur(8px);border-bottom:1px solid #222;
 padding:0 16px;display:flex;gap:14px;align-items:center}
.tab{padding:5px 12px;border:1px solid #2a2a30;border-radius:5px;
 cursor:pointer;color:#888;user-select:none}
.tab.on{background:#1c1c22;color:#e8e8ee;border-color:#444}
.spacer{flex:1}
.meta{color:#666;font-size:12px}
button.rf{background:none;border:1px solid #2a2a30;color:#888;
 border-radius:5px;padding:5px 10px;cursor:pointer;font:inherit}
button.rf:hover{color:#ddd;border-color:#444}
#grid{display:grid;gap:12px;padding:16px;
 grid-template-columns:repeat(auto-fill,minmax(240px,1fr))}
.card{background:#151519;border:1px solid #222;border-radius:7px;
 overflow:hidden;cursor:pointer;transition:border-color .12s}
.card:hover{border-color:#4a4a55}
.thumb{aspect-ratio:4/3;background:#000;display:flex;
 align-items:center;justify-content:center;overflow:hidden}
.thumb img,.thumb video{width:100%;height:100%;object-fit:contain;
 image-rendering:pixelated}
.cap{padding:7px 9px;font-size:11px;color:#9a9aa2;
 white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.cap span{color:#5c5c66}
#tablewrap{padding:0 16px 16px}
table{width:100%;border-collapse:collapse;font-size:12px}
thead th{position:sticky;top:48px;z-index:2;background:#0d0d0f;text-align:left;
 padding:8px 10px;color:#888;cursor:pointer;user-select:none;
 border-bottom:1px solid #2a2a30;white-space:nowrap}
thead th:hover{color:#ddd}
thead th .ar{color:#5a5a66;font-size:10px}
tbody td{padding:6px 10px;border-bottom:1px solid #181820;white-space:nowrap}
tbody tr{cursor:pointer}
tbody tr:hover{background:#16161c}
.c-name{color:#c8c8cc}
.c-rel{color:#6a6a75;font-size:11px;max-width:440px;overflow:hidden;text-overflow:ellipsis}
.c-size,.c-mtime{color:#9a9aa2;text-align:right;font-variant-numeric:tabular-nums}
.empty{padding:60px;text-align:center;color:#555}
#ov{position:fixed;inset:0;z-index:20;background:#000000f2;display:none;
 flex-direction:column}
#ov.on{display:flex}
#ovbar{padding:10px 16px;display:flex;gap:14px;align-items:center;
 color:#999;font-size:12px;border-bottom:1px solid #1a1a1a}
#ovbody{flex:1;display:flex;align-items:center;justify-content:center;
 overflow:hidden;padding:12px}
#ovbody img,#ovbody video{max-width:100%;max-height:100%;
 image-rendering:pixelated}
.nav{cursor:pointer;padding:4px 10px;border:1px solid #2a2a30;
 border-radius:5px;user-select:none}
.nav:hover{color:#fff;border-color:#555}
a.dl{color:#7a7a88;text-decoration:none}
a.dl:hover{color:#ccc}
</style></head><body>
<header>
  <div class="tab on" data-k="index">index</div>
  <div class="tab" data-k="images">images</div>
  <div class="tab" data-k="videos">videos</div>
  <div class="meta" id="count"></div>
  <div class="spacer"></div>
  <button class="rf" id="refresh">refresh</button>
</header>
<div id="tablewrap">
  <table id="tbl">
    <thead id="thead"><tr>
      <th data-k="name">filename <span class="ar"></span></th>
      <th data-k="rel">path <span class="ar"></span></th>
      <th data-k="size">size <span class="ar"></span></th>
      <th data-k="mtime">modified <span class="ar"></span></th>
    </tr></thead>
    <tbody id="tbody"></tbody>
  </table>
</div>
<div id="grid" style="display:none"></div>
<div id="empty" class="empty" style="display:none">nothing here yet</div>
<div id="ov">
  <div id="ovbar">
    <div class="nav" id="prev">&larr;</div>
    <div class="nav" id="next">&rarr;</div>
    <div id="ovname"></div>
    <div class="spacer"></div>
    <a class="dl" id="ovdl" download>download</a>
    <div class="nav" id="close">esc</div>
  </div>
  <div id="ovbody"></div>
</div>
<script>
let view="index", disp=[], idx=-1, sortKey="mtime", sortDir=-1;

function esc(s){return String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
function imgUrl(r){return "/img/"+r.rel.split("/").map(encodeURIComponent).join("/")+"?t="+r.mtime}
function fileUrl(r){return "/file/"+r.kind+"/"+encodeURIComponent(r.name)+"?t="+r.mtime}
function rowUrl(r){return view==="index"?imgUrl(r):fileUrl(r)}
function rowLabel(r){return view==="index"?(r.name+"  ·  "+r.size_h):(r.name+"  ·  "+r.size)}

async function load(keepScroll){
  const y=window.scrollY;
  let data;
  if(view==="index"){
    const r=await fetch("/api/scan",{cache:"no-store"});
    data=await r.json();
    disp=sortRows(data);
    renderTable();
  }else{
    const r=await fetch("/api/list",{cache:"no-store"});
    const all=await r.json();
    data=all[view]||[];
    disp=data;
    renderGrid();
  }
  document.getElementById("count").textContent=data.length+" files";
  if(keepScroll)window.scrollTo(0,y);
}

function sortRows(arr){
  const k=sortKey,d=sortDir;
  return arr.slice().sort((a,b)=>{
    let x,y;
    if(k==="size"){x=a.size;y=b.size}
    else if(k==="mtime"){x=a.mtime;y=b.mtime}
    else if(k==="name"){x=a.name.toLowerCase();y=b.name.toLowerCase()}
    else {x=a.rel.toLowerCase();y=b.rel.toLowerCase()}
    return x<y?-d:x>y?d:0;
  });
}

function renderTable(){
  const tw=document.getElementById("tablewrap"),g=document.getElementById("grid"),e=document.getElementById("empty");
  g.style.display="none";
  if(!disp.length){tw.style.display="none";e.style.display="block";return}
  tw.style.display="block";e.style.display="none";
  document.querySelectorAll("#thead th").forEach(th=>{
    th.querySelector(".ar").textContent=th.dataset.k===sortKey?(sortDir<0?"▼":"▲"):"";
  });
  document.getElementById("tbody").innerHTML=disp.map((r,i)=>
    '<tr data-i="'+i+'">'
    +'<td class="c-name">'+esc(r.name)+'</td>'
    +'<td class="c-rel" title="'+esc(r.rel)+'">'+esc(r.rel)+'</td>'
    +'<td class="c-size">'+esc(r.size_h)+'</td>'
    +'<td class="c-mtime">'+esc(r.mtime_s)+'</td>'
    +'</tr>').join("");
  document.querySelectorAll("#tbody tr").forEach(tr=>tr.onclick=()=>open_(+tr.dataset.i));
}

function renderGrid(){
  const tw=document.getElementById("tablewrap"),g=document.getElementById("grid"),e=document.getElementById("empty");
  tw.style.display="none";
  if(!disp.length){g.style.display="none";e.style.display="block";return}
  g.style.display="grid";e.style.display="none";
  g.innerHTML=disp.map((r,i)=>{
    const inner=r.kind==="videos"
      ? '<video src="'+fileUrl(r)+'" preload="metadata" muted></video>'
      : '<img src="'+fileUrl(r)+'" loading="lazy">';
    return '<div class="card" data-i="'+i+'"><div class="thumb">'+inner+'</div>'
      +'<div class="cap">'+esc(r.name)+' <span>'+esc(r.size)+' · '+esc(r.age)+'</span></div></div>';
  }).join("");
  g.querySelectorAll(".card").forEach(c=>c.onclick=()=>open_(+c.dataset.i));
}

function open_(i){
  if(i<0||i>=disp.length)return;
  idx=i;const r=disp[i];
  document.getElementById("ovname").textContent=rowLabel(r);
  document.getElementById("ovdl").href=rowUrl(r);
  document.getElementById("ovbody").innerHTML=(view!=="index"&&r.kind==="videos")
    ? '<video src="'+rowUrl(r)+'" controls autoplay loop></video>'
    : '<img src="'+rowUrl(r)+'">';
  document.getElementById("ov").classList.add("on");
}
function close_(){
  document.getElementById("ov").classList.remove("on");
  document.getElementById("ovbody").innerHTML="";idx=-1;
}

document.querySelectorAll(".tab").forEach(t=>t.onclick=()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.remove("on"));
  t.classList.add("on");view=t.dataset.k;idx=-1;load();
});
document.querySelectorAll("#thead th").forEach(th=>th.onclick=()=>{
  const k=th.dataset.k;
  if(sortKey===k)sortDir*=-1;
  else{sortKey=k;sortDir=(k==="mtime"||k==="size")?-1:1}
  disp=sortRows(disp);renderTable();
});
document.getElementById("refresh").onclick=()=>load(true);
document.getElementById("close").onclick=close_;
document.getElementById("prev").onclick=()=>open_(idx-1);
document.getElementById("next").onclick=()=>open_(idx+1);
document.getElementById("ov").onclick=e=>{if(e.target.id==="ovbody")close_()};
document.addEventListener("keydown",e=>{
  if(idx<0)return;
  if(e.key==="Escape")close_();
  if(e.key==="ArrowLeft")open_(idx-1);
  if(e.key==="ArrowRight")open_(idx+1);
});

load();
setInterval(()=>{if(idx<0)load(true)},5000);
</script></body></html>"""


def human(n):
    for u in ("B","KB","MB","GB"):
        if n < 1024: return f"{n:.0f} {u}" if u=="B" else f"{n:.1f} {u}"
        n /= 1024
    return f"{n:.1f} TB"

def ago(t):
    d = int(time.time()-t)
    if d < 60: return f"{d}s ago"
    if d < 3600: return f"{d//60}m ago"
    if d < 86400: return f"{d//3600}h ago"
    return f"{d//86400}d ago"

def _fmt_time(t):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))

def scan():
    rows = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if not d.startswith(".") and d not in SKIP_DIRS]
        for fn in filenames:
            if fn.startswith("."): continue
            if os.path.splitext(fn)[1].lower() not in SCAN_EXT: continue
            fp = os.path.join(dirpath, fn)
            if not os.path.isfile(fp): continue
            try:
                st = os.stat(fp)
            except OSError:
                continue
            rows.append({"name": fn, "rel": os.path.relpath(fp, ROOT),
                         "size": st.st_size, "size_h": human(st.st_size),
                         "mtime": int(st.st_mtime), "mtime_s": _fmt_time(st.st_mtime),
                         "age": ago(st.st_mtime)})
    return rows

def listing():
    out = {}
    for kind, path in DIRS.items():
        exts = IMG_EXT if kind == "images" else VID_EXT
        rows = []
        if os.path.isdir(path):
            for name in os.listdir(path):
                if name.startswith("."): continue
                if os.path.splitext(name)[1].lower() not in exts: continue
                fp = os.path.join(path, name)
                if not os.path.isfile(fp): continue
                st = os.stat(fp)
                rows.append({"name": name, "kind": kind, "mtime": int(st.st_mtime),
                             "size": human(st.st_size), "age": ago(st.st_mtime)})
        rows.sort(key=lambda r: -r["mtime"])
        out[kind] = rows
    return out


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 30  # reap idle keep-alive connections so threads/fds don't accumulate

    def _send(self, body, ctype, code=200, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _safe_path(self, rel):
        fp = os.path.realpath(os.path.join(ROOT, rel))
        root_real = os.path.realpath(ROOT)
        if fp != root_real and not fp.startswith(root_real + os.sep):
            return None
        if not os.path.isfile(fp):
            return None
        return fp

    def do_HEAD(self): self.do_GET(head=True)

    def do_GET(self, head=False):
        p = urllib.parse.urlparse(self.path).path

        if p == "/":
            return self._send(PAGE.encode(), "text/html; charset=utf-8")

        if p == "/api/list":
            return self._send(json.dumps(listing()).encode(), "application/json")

        if p == "/api/scan":
            return self._send(json.dumps(scan()).encode(), "application/json")

        m = re.match(r"^/img/(.+)$", p)
        if m:
            fp = self._safe_path(urllib.parse.unquote(m.group(1)))
            if not fp:
                return self._send(b"not found", "text/plain", 404)
            if os.path.splitext(fp)[1].lower() == ".ppm":
                try:
                    st = os.stat(fp)
                    body = _ppm_png(fp, int(st.st_mtime))
                except Exception:
                    body = _ERROR_PNG
                return self._send(body, "image/png")
            ctype = mimetypes.guess_type(fp)[0] or "application/octet-stream"
            with open(fp, "rb") as f:
                body = f.read()
            return self._send(body, ctype)

        m = re.match(r"^/file/(images|videos)/(.+)$", p)
        if not m:
            return self._send(b"not found", "text/plain", 404)

        kind = m.group(1)
        name = posixpath.basename(urllib.parse.unquote(m.group(2)))
        fp = os.path.join(DIRS[kind], name)
        if not os.path.isfile(fp) or not os.path.realpath(fp).startswith(
                os.path.realpath(DIRS[kind]) + os.sep):
            return self._send(b"not found", "text/plain", 404)

        ctype = mimetypes.guess_type(fp)[0] or "application/octet-stream"
        size = os.path.getsize(fp)
        rng = self.headers.get("Range")

        start, end = 0, size - 1
        code = 200
        if rng:
            mm = re.match(r"bytes=(\d*)-(\d*)", rng)
            if mm:
                s, e = mm.group(1), mm.group(2)
                if s:
                    start = int(s)
                    if e: end = min(int(e), size - 1)
                else:
                    start = max(0, size - int(e))
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers(); return
                code = 206

        length = end - start + 1
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head: return
        with open(fp, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(262144, left))
                if not chunk: break
                try: self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError): return
                left -= len(chunk)

    def log_message(self, *a): pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128

if __name__ == "__main__":
    for k, v in DIRS.items():
        print(f"{k:8} {v}  {'ok' if os.path.isdir(v) else 'MISSING'}")
    print(f"root     {ROOT}")
    print(f"serving on http://{BIND}:{PORT}")
    Server((BIND, PORT), H).serve_forever()
