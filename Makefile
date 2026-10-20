CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGETS = db2_inspector db2_select

all: $(TARGETS)

db2_inspector: db2_inspector.c
	$(CC) $(CFLAGS) -o db2_inspector db2_inspector.c

db2_select: db2_select.c
	$(CC) $(CFLAGS) -o db2_select db2_select.c

clean:
	rm -f $(TARGETS)
