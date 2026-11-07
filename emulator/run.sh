#!/bin/sh
# Captura de pantalla de una app en el emulador de InkView (x86-64, GTK) dentro de Docker.
#   emulator/prepare.py                         # una vez: sistema y datos de ejemplo
#   emulator/run.sh apps/pockit captura.png [segundos]      (una carpeta: compila todos sus .c)
# APPARGS="…" pasa argumentos a la app (cada app documenta los suyos en main()). PK_LANG=en o es fuerza el idioma.
# PANTALLA=788x1576 simula otra pantalla (esa es la del PocketBook Q; por defecto 1072x1448, la del Verse Pro).
set -e
SRC=$1; OUT=$2; WAIT=${3:-5}
if [ -d "$SRC" ]; then SRCS=$(ls "$SRC"/*.c | sed 's#^#/w/#' | tr '\n' ' '); else SRCS="/w/$SRC"; fi
cd "$(dirname "$0")/.."
docker image inspect pb-emu >/dev/null 2>&1 || docker build -q -t pb-emu emulator >/dev/null
docker run --rm -e APPARGS="$APPARGS" -e PK_LANG="$PK_LANG" -e PANTALLA="$PANTALLA" -v "$PWD":/w -w /w/emulator/rt pb-emu sh -c "
set -e
if [ -n \"\$PANTALLA\" ]; then
  cp -a /w/emulator/rt /tmp/rt && cd /tmp/rt
  # el emulador solo conoce lectores de su SDK: se usa uno grande (InkPad 3, 1404x1872, también 300 ppp)
  # y la app cree que la pantalla mide PANTALLA; la captura se recorta a ese tamaño
  sed -i \"s/^emu_device=.*/emu_device=740/; s/^emu_width=.*/emu_width=1404/; s/^emu_height=.*/emu_height=1872/\" system/config/global.cfg
  ln -sf hwconfig_740 system/config/hwconfig
  printf 'int ScreenWidth(void){return %s;}\\nint ScreenHeight(void){return %s;}\\n' \${PANTALLA%x*} \${PANTALLA#*x} > /tmp/pantalla.c
  SRCS=\"/tmp/pantalla.c\"
fi
gcc -O1 -DEMULATOR -DPLATFORM_NX -fsigned-char -std=gnu11 -w -I/w/vendor/libinkview/examples/shim \
    -I/w/vendor/libinkview/sdk/6.10/B288/include -o /tmp/app $SRCS \$SRCS ./libinkview.so ./libhwconfig.so -lm -ldl
export LD_LIBRARY_PATH=\$PWD DISPLAY=:1
Xvfb :1 -screen 0 1600x2000x24 >/dev/null 2>&1 &
sleep 1
/tmp/app \$APPARGS > /tmp/app.log 2>&1 &
sleep $WAIT
import -window root /tmp/raw.png
if [ -n \"\$PANTALLA\" ]; then convert /tmp/raw.png -trim +repage -crop \"\$((\${PANTALLA%x*}/2))x\$((\${PANTALLA#*x}/2))+0+0\" +repage -resize 200% /w/$OUT; else convert /tmp/raw.png -trim +repage /w/$OUT; fi
cp /tmp/raw.png /w/emulator/out/ultima-cruda.png
" 2>&1 | { grep -v "WARNING: The requested image" || true; }
