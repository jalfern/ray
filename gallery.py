#!/usr/bin/env python3
import os, re, json, glob, mimetypes, posixpath, urllib.parse
import http.server, socketserver

PORT = 8080
BIND = "0.0.0.0"

DIRS = {
    "images": "/Users/jon/Dev/ray/images",
    "videos": "/Users/jon/Dev/ray/videos",
}

IMG_EXT = {".png",".jpg",".jpeg",".gif",".webp",".bmp",".tif",".tiff",".avif"}
VID_EXT = {".mp4",".mov",".webm",".m4v",".mkv",".avi"}

mimetypes.add_type("video/quicktime", ".mov")
mimetypes.add_type("video/x-matroska", ".mkv")
mimetypes.add_type("video/mp4", ".m4v")
mimetypes.add_type("image/avif", ".avif")

PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ray output</title><style>
*{box-sizing:border-box}
body{margin:0;background:#0d0d0f;color:#c8c8cc;
 font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
header{position:sticky;top:0;z-index:5;background:#0d0d0fee;
 backdrop-filter:blur(8px);border-bottom:1px solid #222;
 padding:10px 16px;display:flex;gap:14px;align-items:center}
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
  <div class="tab on" data-k="images">images</div>
  <div class="tab" data-k="videos">videos</div>
  <div class="meta" id="count"></div>
  <div class="spacer"></div>
  <button class="rf" id="refresh">refresh</button>
</header>
<div id="grid"></div>
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
let kind="images", items=[], idx=-1;

function url(it){return "/file/"+it.kind+"/"+encodeURIComponent(it.name)+"?t="+it.mtime}

async function load(keepScroll){
  const y=window.scrollY;
  const r=await fetch("/api/list",{cache:"no-store"});
  const all=await r.json();
  items=all[kind]||[];
  const g=document.getElementById("grid");
  document.getElementById("count").textContent=items.length+" files";
  if(!items.length){g.innerHTML='<div class="empty">nothing here yet</div>';return}
  g.innerHTML=items.map((it,i)=>{
    const inner = it.kind==="videos"
      ? '<video src="'+url(it)+'" preload="metadata" muted></video>'
      : '<img src="'+url(it)+'" loading="lazy">';
    return '<div class="card" data-i="'+i+'"><div class="thumb">'+inner+'</div>'
      +'<div class="cap">'+it.name+' <span>'+it.size+' · '+it.age+'</span></div></div>';
  }).join("");
  g.querySelectorAll(".card").forEach(c=>
    c.onclick=()=>open_(parseInt(c.dataset.i)));
  if(keepScroll)window.scrollTo(0,y);
}

function open_(i){
  if(i<0||i>=items.length)return;
  idx=i;const it=items[i];
  document.getElementById("ovname").textContent=it.name+"  ·  "+it.size;
  document.getElementById("ovdl").href=url(it);
  document.getElementById("ovbody").innerHTML = it.kind==="videos"
    ? '<video src="'+url(it)+'" controls autoplay loop></video>'
    : '<img src="'+url(it)+'">';
  document.getElementById("ov").classList.add("on");
}
function close_(){
  document.getElementById("ov").classList.remove("on");
  document.getElementById("ovbody").innerHTML="";idx=-1;
}

document.querySelectorAll(".tab").forEach(t=>t.onclick=()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.remove("on"));
  t.classList.add("on");kind=t.dataset.k;load();
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
    import time
    d = int(time.time()-t)
    if d < 60: return f"{d}s ago"
    if d < 3600: return f"{d//60}m ago"
    if d < 86400: return f"{d//3600}h ago"
    return f"{d//86400}d ago"

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

    def _send(self, body, ctype, code=200, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self): self.do_GET(head=True)

    def do_GET(self, head=False):
        p = urllib.parse.urlparse(self.path).path

        if p == "/":
            return self._send(PAGE.encode(), "text/html; charset=utf-8")

        if p == "/api/list":
            return self._send(json.dumps(listing()).encode(), "application/json")

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

if __name__ == "__main__":
    for k, v in DIRS.items():
        print(f"{k:8} {v}  {'ok' if os.path.isdir(v) else 'MISSING'}")
    print(f"serving on http://{BIND}:{PORT}")
    Server((BIND, PORT), H).serve_forever()
