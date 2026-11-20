# Build: make            (inside the armymen-re:2 image; output ddraw.dll)
CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function
LDFLAGS = -shared -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
LIBS    = -ld3d11 -lgdi32 -luser32 -lole32 -lwindowscodecs -lmfplat -lmfreadwrite -lmfuuid

SRC = ddraw.c surface.c present.c view.c layout.c patches.c mouse.c clock.c window.c hud.c fonts.c controls.c menu.c gpu.c gameplay.c weapons.c bullets.c fog.c objectives.c panel.c roofs.c hdart.c hdsprites.c hdworld.c hdvideo.c tips.c resize.c zoom.c par.c pathfind.c debugview.c screenbtn.c modern.c modmenu.c framerate.c menutext.c radio.c missionpath.c capture.c minimap.c vehicles.c

ddraw.dll: $(SRC) common.h ddraw.def
	$(CC) $(CFLAGS) -o $@ $(SRC) ddraw.def $(LDFLAGS) $(LIBS)

clean:
	rm -f ddraw.dll
