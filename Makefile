CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g -Ivendor -Isrc/common
LDFLAGS =

VENDOR_H = vendor/cJSON.h
VENDOR_C = vendor/cJSON.c

COMMON_SRC = src/common/proto.c $(VENDOR_C)
COMMON_OBJ = build/proto.o build/cJSON.o

TARGETS = bin/gesfich bin/gesprog bin/ejecutor bin/ctrllt

.PHONY: all clean vendor

all: vendor $(TARGETS)

# ── Descargar cJSON si no existe ─────────────────────────────────────────────
vendor:
	@if [ ! -f vendor/cJSON.h ]; then \
		echo "Descargando cJSON..."; \
		curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o vendor/cJSON.h; \
		curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o vendor/cJSON.c; \
	fi

# ── Directorios de salida ─────────────────────────────────────────────────────
build bin:
	mkdir -p $@

# ── Objetos comunes ───────────────────────────────────────────────────────────
build/proto.o: src/common/proto.c src/common/proto.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/cJSON.o: $(VENDOR_C) $(VENDOR_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

# ── gesfich ──────────────────────────────────────────────────────────────────
build/gesfich.o: src/gesfich/gesfich.c src/common/proto.h $(VENDOR_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

bin/gesfich: build/gesfich.o $(COMMON_OBJ) | bin
	$(CC) $(LDFLAGS) $^ -o $@

# ── gesprog ──────────────────────────────────────────────────────────────────
build/gesprog.o: src/gesprog/gesprog.c src/common/proto.h $(VENDOR_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

bin/gesprog: build/gesprog.o $(COMMON_OBJ) | bin
	$(CC) $(LDFLAGS) $^ -o $@

# ── ejecutor ─────────────────────────────────────────────────────────────────
build/ejecutor.o: src/ejecutor/ejecutor.c src/common/proto.h $(VENDOR_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

bin/ejecutor: build/ejecutor.o $(COMMON_OBJ) | bin
	$(CC) $(LDFLAGS) $^ -o $@

# ── ctrllt ───────────────────────────────────────────────────────────────────
build/ctrllt.o: src/ctrllt/ctrllt.c src/common/proto.h $(VENDOR_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

bin/ctrllt: build/ctrllt.o $(COMMON_OBJ) | bin
	$(CC) $(LDFLAGS) $^ -o $@

# ── Limpieza ──────────────────────────────────────────────────────────────────
clean:
	rm -rf build bin
