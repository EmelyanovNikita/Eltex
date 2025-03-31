# Compiler settings
CC = gcc
CFLAGS = -Wall -g -fPIC
LDFLAGS = 

# PJSIP paths (adjust if needed)
PJ_INCLUDE = /usr/local/include
PJ_LIB = /usr/local/lib

# Source files
SRC = auto_answer.c
OBJ = $(SRC:.c=.o)
TARGET = auto_answer

# Libraries
LIBS = -lpjsua2 -lpjsua -lpjsip-ua -lpjsip-simple -lpjsip \
       -lpjmedia-codec -lpjmedia-videodev -lpjmedia-audiodev \
       -lpjmedia -lpjnath -lpjlib-util -lpj \
       -lssl -lcrypto -lm -lstdc++ -lsrtp2 -lilbc -lspeex -lgsm

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -L$(PJ_LIB) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -I$(PJ_INCLUDE) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean