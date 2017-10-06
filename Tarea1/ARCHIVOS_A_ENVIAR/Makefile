CFLAGS= -g -Wall -pedantic -std=c99
LDFLAGS= -g -pthread

all: bwcs

bwcs.o Data-tcp.o: Data.h
bwcs.o jsocket6.4.o: jsocket6.4.h
bwcs: bwcs.o Data-tcp.o jsocket6.4.o
