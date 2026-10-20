CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGET  = db2_inspector

all: $(TARGET)

$(TARGET): db2_inspector.c
	$(CC) $(CFLAGS) -o $(TARGET) db2_inspector.c

clean:
	rm -f $(TARGET)
