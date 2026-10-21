# Compila las apps de Pockit para los lectores PocketBook con `zig cc`, sin el SDK oficial.
#   make            → dist/B288 (Verse, Verse Pro, Era, InkPad…) y dist/RK3566 (PocketBook Q)
#   make b288       → solo una plataforma
# Las cabeceras y librerías de InkView se descargan de github.com/ssttevee/libinkview.

APPS        := bolsillo cuaderno ia lectura
INKVIEW     := vendor/libinkview
INKVIEW_REF := 5206688e8e4f70304629304bddeca32e306f0967
CFLAGS      := -O2 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-unused-result \
               -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -fsigned-char -std=gnu11 -s

# Nombre del .app en el lector
NAME_bolsillo := Bolsillo
NAME_cuaderno := Cuaderno
NAME_ia       := IA
NAME_lectura  := Lectura

all: b288 rk3566 manifest

$(INKVIEW):
	git clone -q https://github.com/ssttevee/libinkview.git $(INKVIEW)
	git -C $(INKVIEW) checkout -q $(INKVIEW_REF)

# B288: firmware 6.10, userland de 32 bits soft-float
b288: $(INKVIEW)
	@mkdir -p dist/B288
	@for a in $(APPS); do \
	  zig cc -target arm-linux-gnueabi.2.23 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.10/B288/include \
	    -o dist/B288/$$a.app apps/$$a/$$a.c $(INKVIEW)/sdk/6.10/B288/lib/libinkview.so || exit 1; \
	  echo "B288   $$a"; done

# RK3566 (PocketBook Q): firmware 6.11, hard-float
rk3566: $(INKVIEW)
	@mkdir -p dist/RK3566
	@for a in $(APPS); do \
	  zig cc -target arm-linux-gnueabihf.2.34 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.11/RK3566/include \
	    -o dist/RK3566/$$a.app apps/$$a/$$a.c $(INKVIEW)/sdk/6.11/RK3566/lib/libinkview.so || exit 1; \
	  echo "RK3566 $$a"; done

manifest:
	@mkdir -p dist/icons
	@for a in $(APPS); do cp apps/$$a/icon.bmp dist/icons/$$a.bmp; cp apps/$$a/icon_f.bmp dist/icons/$${a}_f.bmp; done
	@cp apps.json dist/apps.json

clean:
	rm -rf dist

.PHONY: all b288 rk3566 manifest clean
