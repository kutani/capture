BIN = capture
OBJS = capture.o
OPTIM = -O0
DEBUG = -g -Wall -Wextra
CFLAGS = $(DEBUG) $(OPTIM)
LFLAGS = $(OPTIM) $(DEBUG) -lX11 -lXext -lrt -lm -lpthread
LINK = cc

all: $(OBJS)
	$(LINK) $(LFLAGS) $(OBJS) -o $(BIN)

clean:
	rm *.o
	rm $(BIN)
