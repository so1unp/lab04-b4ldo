CC=gcc
BIN=./bin
CFLAGS=-g -Wall -Wextra -Wshadow -Wconversion -Wunreachable-code -Iinclude -Itools
LDFLAGS=-lrt -pthread -lm

# Fuentes y objetos compartidos
SRCS_SHARED=model/mapa.c tools/movement.c tools/barra.c

.PHONY: all
all: $(BIN)/servidor $(BIN)/nave $(BIN)/estacion

$(BIN)/servidor: servidor.c $(SRCS_SHARED)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

$(BIN)/nave: src/nave.c $(SRCS_SHARED)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -lncurses

$(BIN)/estacion: src/estacion.c $(SRCS_SHARED)
	@mkdir -p $(BIN)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean

zip:
	git archive --format zip --output ${USER}-lab03.zip HEAD

html:
	pandoc -o README.html -f gfm README.md
