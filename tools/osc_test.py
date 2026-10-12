"""Test du serveur OSC de ToneForge (lancer l'appli autonome d'abord). Sans dépendance : OSC encodé à la main."""
import socket, struct, sys, time

def pad(b): return b + b"\0" * (4 - len(b) % 4)
def s(x): return pad(x.encode())

def enc(addr, *args):
    tags, data = ",", b""
    for a in args:
        if isinstance(a, float): tags += "f"; data += struct.pack(">f", a)
        elif isinstance(a, int): tags += "i"; data += struct.pack(">i", a)
        else: tags += "s"; data += s(a)
    return s(addr) + s(tags) + data

def dec(p):
    def rs(i):
        j = p.index(b"\0", i); return p[i:j].decode("utf-8", "replace"), (j + 4) & ~3
    addr, i = rs(0); tags, i = rs(i); out = []
    for t in tags[1:]:
        if t == "f": out.append(struct.unpack(">f", p[i:i+4])[0]); i += 4
        elif t == "i": out.append(struct.unpack(">i", p[i:i+4])[0]); i += 4
        elif t == "s": v, i = rs(i); out.append(v)
    return addr, out

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
rx.bind(("127.0.0.1", 9001)); rx.settimeout(0.2)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def send(*a): tx.sendto(enc(*a), ("127.0.0.1", 9000))

def collect(sec):
    msgs, end = [], time.time() + sec
    while time.time() < end:
        try: msgs.append(dec(rx.recv(65536)))
        except socket.timeout: pass
    return msgs

ok = True
def check(name, cond, info=""):
    global ok; ok &= bool(cond); print(("OK  " if cond else "ÉCHEC ") + name, info)

send("/tf/hello", 9001)
m = collect(1.0)
lv = [a for x, a in m if x == "/tf/levels"]
check("niveaux ~30 Hz", 20 <= len(lv) <= 40, f"{len(lv)}/s, dernier {lv[-1] if lv else None}")
params = {a[0]: a[1] for x, a in m if x == "/tf/param"}
check("état des paramètres", len(params) > 20, f"{len(params)} params, output={params.get('output')}")
loaded = {a[0]: a[1] for x, a in m if x == "/tf/loaded"}
check("fichiers chargés", set(loaded) == {"amp", "pedal", "ir"}, str(loaded))

old = params.get("output", 0.0)
send("/tf/param", "output", -6.0)
m = collect(0.3)
check("param output -6 dB renvoyé", any(x == "/tf/param" and a == ["output", -6.0] for x, a in m))
send("/tf/param", "output", float(old))
send("/tf/param", "nimporte", 1.0)
check("param inconnu -> /tf/error", any(x == "/tf/error" for x, a in collect(0.3)))

send("/tf/library")
m = collect(1.5)
raw = [a for x, a in m if x == "/tf/lib/item"]
check("index de bibliothèque uniques et continus", sorted(a[0] for a in raw) == list(range(len(raw))))
items = [a[1:] for a in raw]
begin = [a[0] for x, a in m if x == "/tf/lib/begin"]
check("bibliothèque complète", begin and len(items) == begin[0] and any(x == "/tf/lib/end" for x, _ in m),
      f"{len(items)}/{begin[0] if begin else '?'} — " + ", ".join(sorted({f'{k}:{sum(1 for i in items if i[0]==k)}' for k in ('amp','pedal','ir')})))
if items: print("   ex.", items[0][:4])

for kind in ("amp", "pedal"):
    cand = [i for i in items if i[0] == kind and i[4] != loaded.get(kind)]
    if cand:
        send(f"/tf/load/{kind}", cand[0][4])
        m = collect(1.5)
        check(f"chargement {kind}", any(x == "/tf/loaded" and a == [kind, cand[0][4]] for x, a in m), cand[0][3])
        if loaded.get(kind): send(f"/tf/load/{kind}", loaded[kind]); collect(1.0)   # remettre l'original
send("/tf/hello", 0)
check("port invalide -> /tf/error, connexion gardée", any(x == "/tf/error" for x, a in collect(0.3)))
send("/tf/load/amp", "C:/inexistant.nam")
check("chargement invalide -> /tf/error", any(x == "/tf/error" for x, a in collect(0.5)))
sys.exit(0 if ok else 1)
