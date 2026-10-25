# Compila Pockit (una app con módulos) y el Modo bolsillo para los lectores PocketBook con `zig cc`, sin el SDK oficial.
#   make            → dist/B288 (Verse, Verse Pro, Era, InkPad…) y dist/RK3566 (PocketBook Q)
#   make b288       → solo una plataforma
# Las cabeceras y librerías de InkView se descargan de github.com/ssttevee/libinkview.

POCKIT_SRC  := $(wildcard apps/pockit/*.c)
INKVIEW     := vendor/libinkview
INKVIEW_REF := 5206688e8e4f70304629304bddeca32e306f0967
CFLAGS      := -O2 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-unused-result \
               -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -fsigned-char -std=gnu11 -s


all: b288 rk3566 manifest

$(INKVIEW):
	git clone -q https://github.com/ssttevee/libinkview.git $(INKVIEW)
	git -C $(INKVIEW) checkout -q $(INKVIEW_REF)

# B288: firmware 6.10, userland de 32 bits soft-float
b288: $(INKVIEW)
	@mkdir -p dist/B288
	zig cc -target arm-linux-gnueabi.2.23 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.10/B288/include \
	  -o dist/B288/pockit.app $(POCKIT_SRC) $(INKVIEW)/sdk/6.10/B288/lib/libinkview.so -lm
	zig cc -target arm-linux-gnueabi.2.23 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.10/B288/include \
	  -o dist/B288/bolsillo.app apps/bolsillo/bolsillo.c $(INKVIEW)/sdk/6.10/B288/lib/libinkview.so

# RK3566 (PocketBook Q): firmware 6.11, hard-float
rk3566: $(INKVIEW)
	@mkdir -p dist/RK3566
	zig cc -target arm-linux-gnueabihf.2.34 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.11/RK3566/include \
	  -o dist/RK3566/pockit.app $(POCKIT_SRC) $(INKVIEW)/sdk/6.11/RK3566/lib/libinkview.so -lm
	zig cc -target arm-linux-gnueabihf.2.34 $(CFLAGS) -I$(INKVIEW)/examples/shim -I$(INKVIEW)/sdk/6.11/RK3566/include \
	  -o dist/RK3566/bolsillo.app apps/bolsillo/bolsillo.c $(INKVIEW)/sdk/6.11/RK3566/lib/libinkview.so

manifest:
	@mkdir -p dist/icons
	@cp apps/pockit/icons/pockit.bmp apps/pockit/icons/pockit_f.bmp dist/icons/
	@cp apps/bolsillo/icon.bmp dist/icons/bolsillo.bmp; cp apps/bolsillo/icon_f.bmp dist/icons/bolsillo_f.bmp
	@cp apps.json dist/apps.json

clean:
	rm -rf dist

.PHONY: all b288 rk3566 manifest clean
