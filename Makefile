CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99

all: jtext

jtext: jtext.c
	$(CC) -o jtext jtext.c $(CFLAGS)