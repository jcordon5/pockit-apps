#!/bin/sh
# Captura de pantalla de una app en el emulador de InkView (x86-64, GTK) dentro de Docker.
#   emulator/prepare.py                         # una vez: sistema y datos de ejemplo
#   emulator/run.sh apps/cuaderno/cuaderno.c captura.png [segundos]
# APPARGS="…" pasa argumentos a la app (cada app documenta los suyos en main()).
set -e
SRC=$1; OUT=$2; WAIT=${3:-5}
cd "$(dirname "$0")/.."
docker image inspect pb-emu >/dev/null 2>&1 || docker build -q -t pb-emu emulator >/dev/null
docker run --rm -e APPARGS="$APPARGS" -v "$PWD":/w -w /w/emulator/rt pb-emu sh -c "
set -e
gcc -O1 -DEMULATOR -DPLATFORM_NX -fsigned-char -std=gnu11 -w -I/w/vendor/libinkview/examples/shim \
    -I/w/vendor/libinkview/sdk/6.10/B288/include -o /tmp/app /w/$SRC ./libinkview.so ./libhwconfig.so -lm -ldl
export LD_LIBRARY_PATH=/w/emulator/rt DISPLAY=:1
Xvfb :1 -screen 0 1600x2000x24 >/dev/null 2>&1 &
sleep 1
/tmp/app \$APPARGS > /tmp/app.log 2>&1 &
sleep $WAIT
import -window root png:- | convert - -trim +repage /w/$OUT
" 2>&1 | grep -v 'WARNING: The requested image'
