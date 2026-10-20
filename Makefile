CC      = gcc
CFLAGS  = -O2 -fno-strict-aliasing -Wall -Wextra -std=c11
TARGETS = db2_inspector db2_select db2_rename_idx

all: $(TARGETS)

db2_inspector: db2_inspector.c
	$(CC) $(CFLAGS) -o db2_inspector db2_inspector.c

db2_select: db2_select.c
	$(CC) $(CFLAGS) -o db2_select db2_select.c

db2_rename_idx: db2_rename_idx.c
	$(CC) $(CFLAGS) -o db2_rename_idx db2_rename_idx.c

clean:
	rm -f $(TARGETS)
