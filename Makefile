CC = gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wno-unused-parameter -fno-asm
INCLUDES = -Iinclude
LIBS = -lcrypto

# Detect OS for OpenSSL linking
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix openssl)
    INCLUDES += -I$(BREW_PREFIX)/include
    LIBS += -L$(BREW_PREFIX)/lib
endif

SRCDIR = src
OBJDIR = obj
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

CLIENT_SOURCES = $(filter-out $(SRCDIR)/server.c, $(SOURCES))
SERVER_SOURCES = $(filter-out $(SRCDIR)/client.c, $(SOURCES))
CLIENT_OBJECTS = $(CLIENT_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
SERVER_OBJECTS = $(SERVER_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

all: client server

client: $(CLIENT_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

server: $(SERVER_OBJECTS)  
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) client server clientlog.txt serverlog.txt *.dat

.PHONY: all clean