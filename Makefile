# Makefile for myshell project
# Using separate compilation for modularity and efficiency

# compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -D_GNU_SOURCE

# target executable
TARGET = myshell

# object files
OBJS = myshell.o parser.o executor.o builtins.o
SERVER_OBJS = parser.o executor.o builtins.o scheduler_queue.o scheduler.o
# default target - builds the executable
all: $(TARGET)

# linking object files to create executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# compiling myshell.c to myshell.o
myshell.o: myshell.c myshell.h
	$(CC) $(CFLAGS) -c myshell.c

# compiling parser.c to parser.o
parser.o: parser.c myshell.h
	$(CC) $(CFLAGS) -c parser.c

# compiling executor.c to executor.o
executor.o: executor.c myshell.h
	$(CC) $(CFLAGS) -c executor.c

# compiling builtins.c to builtins.o
builtins.o: builtins.c myshell.h
	$(CC) $(CFLAGS) -c builtins.c

scheduler_queue.o: scheduler_queue.c scheduler_queue.h server_shared.h
	$(CC) $(CFLAGS) -c scheduler_queue.c

scheduler.o: scheduler.c scheduler.h scheduler_queue.h server_shared.h
	$(CC) $(CFLAGS) -c scheduler.c

# ===== SERVER TARGET (FIXED) =====
server: server.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o server server.c $(SERVER_OBJS)
# compiling and linking client program
client: client.c
	$(CC) $(CFLAGS) -o client client.c

# compiling demo test program
demo: demo.c
	$(CC) $(CFLAGS) -o demo demo.c
# cleaning build artifacts
clean:
	rm -f $(OBJS) $(TARGET) server client demo scheduler_queue.o scheduler.o


# rebuilding from scratch
rebuild: clean all

# phony targets (not actual files)
.PHONY: all clean rebuild server client
