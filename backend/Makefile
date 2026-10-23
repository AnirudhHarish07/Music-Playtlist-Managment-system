# Music Playlist server - works with GCC on Linux and MinGW on Windows.
CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c99 -O2
# Mongoose is third-party code: needs POSIX extensions (gnu99), no strict warnings.
MGFLAGS := -std=gnu99 -O2 -w

ifeq ($(OS),Windows_NT)
  EXE  := server.exe
  TEST := test_playlist.exe
  LIBS := -lws2_32
  RM   := del /Q
else
  EXE  := server
  TEST := test_playlist
  LIBS :=
  RM   := rm -f
endif

all: $(EXE)

$(EXE): server.o playlist.o mongoose.o
	$(CC) -o $@ $^ $(LIBS)

server.o: server.c playlist.h mongoose.h
	$(CC) $(CFLAGS) -c server.c

playlist.o: playlist.c playlist.h
	$(CC) $(CFLAGS) -c playlist.c

mongoose.o: mongoose.c mongoose.h
	$(CC) $(MGFLAGS) -c mongoose.c

# Linked list unit tests (no networking involved)
test: $(TEST)
	./$(TEST)

$(TEST): test_playlist.c playlist.c playlist.h
	$(CC) $(CFLAGS) -g -o $@ test_playlist.c playlist.c

run: $(EXE)
	./$(EXE)

clean:
	$(RM) *.o $(EXE) $(TEST)

.PHONY: all test run clean
