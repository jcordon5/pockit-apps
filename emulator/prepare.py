#!/usr/bin/env python3
"""Prepara emulator/rt: el sistema del emulador de InkView y datos de ejemplo (libros de
dominio público, notas y un chat) para hacer capturas sin un lector de verdad."""
import os, shutil, sqlite3, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RT = os.path.join(HERE, "rt")
MIRROR = os.path.join(ROOT, "vendor", "pb-sdk-mirror")
EXT1 = os.path.join(RT, "system", "mnt", "ext1")

if not os.path.isdir(MIRROR):
    subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--sparse",
                    "https://github.com/Sean-on-Git/PocketBook-SDK.git", MIRROR], check=True)
    subprocess.run(["git", "-C", MIRROR, "sparse-checkout", "set", "system"], check=True)
shutil.rmtree(RT, ignore_errors=True)
shutil.copytree(os.path.join(MIRROR, "system"), os.path.join(RT, "system"))
for f in ("libinkview.so", "libhwconfig.so"):
    shutil.copy(os.path.join(ROOT, "vendor", "libinkview", "sdk", "6.10", "B288", "emulator-x86_64", f), RT)

# fuentes que trae el espejo y pantalla de 1072x1448
cfg = os.path.join(RT, "system", "config", "global.cfg")
lines = [l for l in open(cfg, encoding="utf-8").read().splitlines() if l and not l.startswith("#")]
lines = [l.replace("Roboto-", "LiberationSans-").replace("LiberationSans-Regular", "LiberationSans") for l in lines]
lines += ["emu_width=1072", "emu_height=1448", "emu_compress_mode=0"]
open(cfg, "w", encoding="utf-8").write("\n".join(lines) + "\n")

now = int(time.time())
day = 86400
os.makedirs(os.path.join(EXT1, "system", "explorer-3"), exist_ok=True)
os.makedirs(os.path.join(EXT1, "system", "config"), exist_ok=True)

# biblioteca: (título, autor, página, total, días desde la última apertura, terminado)
books = [
    ("Don Quijote de la Mancha", "Miguel de Cervantes", 412, 1104, 0, 0),
    ("Niebla", "Miguel de Unamuno", 61, 196, 2, 0),
    ("Fortunata y Jacinta", "Benito Pérez Galdós", 233, 1035, 5, 0),
    ("La Regenta", "Leopoldo Alas «Clarín»", 820, 820, 21, 1),
    ("Rimas y leyendas", "Gustavo Adolfo Bécquer", 160, 160, 34, 1),
    ("Los pazos de Ulloa", "Emilia Pardo Bazán", 0, 0, 0, 0),
    ("Luces de bohemia", "Ramón María del Valle-Inclán", 12, 140, 70, 0),
]
db = sqlite3.connect(os.path.join(EXT1, "system", "explorer-3", "explorer-3.db"))
db.executescript("""
create table books_impl(id integer primary key, title, author, ts_added, hidden default 0);
create table folders(id integer primary key, name);
create table files(id integer primary key, folder_id, book_id, filename);
create table books_settings(bookid, profileid default 1, cpage, npage, opentime, completed, completed_ts);
insert into folders values (1, '/mnt/ext1/Libros');
""")
for i, (t, a, p, n, ago, done) in enumerate(books, 1):
    opened = now - ago * day - 3600 if p else 0
    db.execute("insert into books_impl values (?,?,?,?,0)", (i, t, a, now - 90 * day))
    db.execute("insert into files values (?,?,?,?)", (i, 1, i, t + ".epub"))
    db.execute("insert into books_settings values (?,1,?,?,?,?,?)", (i, p, n, opened, done, opened if done else 0))
db.commit()

meta = sqlite3.connect(os.path.join(EXT1, "system", "config", "books.db"))
meta.executescript("""
create table Items(OID integer primary key, ParentID, TypeID);
create table Books(OID integer primary key, Title);
create table TagNames(OID integer primary key, TagName);
create table Tags(OID integer primary key, ItemID, TagID, Val);
insert into TagNames values (1,'bm.type'),(2,'bm.quotation'),(3,'bm.note'),(4,'bm.book_mark');
""")
marks = [
    ("Don Quijote de la Mancha", "highlight", "En un lugar de la Mancha, de cuyo nombre no quiero acordarme, no ha mucho tiempo que vivía un hidalgo de los de lanza en astillero, adarga antigua, rocín flaco y galgo corredor.", "", 0),
    ("Don Quijote de la Mancha", "highlight", "La libertad, Sancho, es uno de los más preciosos dones que a los hombres dieron los cielos.", "Para releer.", 1),
    ("Niebla", "highlight", "La vida es una nebulosa.", "", 2),
    ("Don Quijote de la Mancha", "note", "hidalgo", '{"context":"no ha mucho tiempo que vivía un hidalgo de los de lanza en astillero","text":"[iˈðalɣo] sustantivo 1. Persona que por su sangre pertenecía a la nobleza no titulada. 2. Persona de ánimo generoso y noble. a) Sinónimos: noble, caballero."}', 0),
    ("Don Quijote de la Mancha", "note", "adarga", '{"context":"lanza en astillero, adarga antigua, rocín flaco","text":"[aˈðaɾɣa] sustantivo 1. Escudo de cuero, ovalado o de figura de corazón."}', 0),
    ("Niebla", "note", "nebulosa", '{"context":"La vida es una nebulosa.","text":"[neβuˈlosa] sustantivo 1. Astronomía. Nube de gas y polvo interestelar. 2. Cosa confusa o poco clara."}', 2),
]
titles = {}
oid = 100
for t, *_ in books:
    oid += 1
    titles[t] = oid
    meta.execute("insert into Items values (?,0,1)", (oid,))
    meta.execute("insert into Books values (?,?)", (oid, t))
for t, kind, text, note, ago in marks:
    oid += 1
    meta.execute("insert into Items values (?,?,4)", (oid, titles[t]))
    created = now - ago * day - 1800
    disp = ',"displayType":"translation"' if note.startswith("{") else ""
    vals = [(1, kind), (2, '{"text":%s}' % __import__("json").dumps(text, ensure_ascii=False)), (3, note),
            (4, '{"created":%d%s}' % (created, disp))]
    for tag, v in vals:
        meta.execute("insert into Tags(ItemID,TagID,Val) values (?,?,?)", (oid, tag, v))
meta.commit()

# notas y un chat de ejemplo
os.makedirs(os.path.join(EXT1, "Notas"), exist_ok=True)
open(os.path.join(EXT1, "Notas", "1.md"), "w").write("""# Lecturas de otoño
Lo que quiero leer antes de **diciembre**:
1. Terminar *Don Quijote*
2. Empezar *Los pazos de Ulloa*
## Para hacer
- [x] Pasar los subrayados a limpio
- [ ] Buscar la edición ilustrada de Bécquer
- [ ] Club de lectura del jueves
> Leer no es matar el tiempo, es fecundarlo.
""")
open(os.path.join(EXT1, "Notas", "2.md"), "w").write("# Lista de la compra\n- [x] Café en grano\n- [ ] Pan de centeno\n- [ ] Manzanas\n")
os.utime(os.path.join(EXT1, "Notas", "2.md"), (now - 3 * day, now - 3 * day))
os.makedirs(os.path.join(EXT1, "Chats"), exist_ok=True)
open(os.path.join(EXT1, "Chats", "1.md"), "w").write("""# ¿Qué es una adarga?
<!-- coste_usd=0.000041 -->

### Tú

¿Qué es una «adarga antigua» en el primer párrafo del Quijote?

### IA

Una **adarga** es un **escudo de cuero**, ovalado o con forma de corazón, que usaban los jinetes.

Que sea *antigua* es parte de la burla de Cervantes:
- don Quijote se arma con **piezas viejas** de sus bisabuelos;
- en su época ya nadie las llevaba, así que el detalle lo retrata como un caballero **fuera de su tiempo**.

> Lanza en astillero, adarga antigua, rocín flaco y galgo corredor: cuatro pinceladas de un hidalgo venido a menos.
""")
print("emulator/rt preparado")
