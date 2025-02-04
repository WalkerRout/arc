CC = gcc
OBJ = bin/arc
OBJS = tests/*.c
CFLAGS = -O1 -g -Wall -Wextra -Wpedantic -Werror
LIBS = -lpthread

all: test

build:
	@$(CC) $(OBJS) $(CFLAGS) $(LIBS) -o $(OBJ)

test: build
	@./$(OBJ)

debug:
	@valgrind -s ./$(OBJ)

clean:
	@rm ./$(OBJ)
	@echo "Cleaned!"
