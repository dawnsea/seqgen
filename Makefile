LIBS = -levent -lpthread
CC = gcc -O2 -g -DREENTRANT


SRC = $(wildcard *.c)
DEBUG = -DDEBUG -DDEBUG_INFO -DDEBUG_DEBUG -Wall
all: $(SRC)
	$(CC) -o seqgen $(SRC) $(DEBUG) $(LIBS)

.SUFFIXES: .c.o

.c.o:
	$(CC) -c -o $@ $< $(DEBUG) $(LIBS)

clean :
	find ./ -name "*.*o" -exec rm -f {} +
	rm -f seqgen
