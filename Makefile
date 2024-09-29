CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99
BIN = jedit

all: $(BIN)

$(BIN): jedit.c
	$(CC) -o $(BIN) jedit.c $(CFLAGS)