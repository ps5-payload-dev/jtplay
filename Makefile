#   Copyright (C) 2026 John Törnblom
#
# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CABUNDLE=$(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt

CFLAGS := -Wall -Werror -DTITLE_ID="\"BREW10003\""
CFLAGS += -DCABUNDLE="\"$(CABUNDLE)\""

SRCS := src/main-ps5.c src/srv.c src/mime.c src/asset.c src/mdns.c src/smb.c
SRCS += src/http.c src/fs.c src/ssdp.c src/viz.c

LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libmicrohttpd --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config microdns --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libsmb2 --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libcurl --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libavcodec --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libavformat --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libswresample --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libswscale --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libavutil --libs`
LDADD  += -lm

all: jtplay-install.elf

src/asset.c: src/asset_bundle.inc

src/asset_bundle.inc:
	$(PYTHON) tools/gen-asset-module.py assets > $@

src/install.c: jtplay-srv.elf sce_sys/param.json sce_sys/icon0.png

jtplay-srv.elf: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)

jtplay-install.elf: src/install.c
	$(CC) $(CFLAGS) -lSceIpmi -lSceAppInstUtil -o $@ $^

clean:
	rm -f *.elf *.o src/asset_bundle.inc

test: jtplay-srv.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

install: jtplay-install.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^ &
