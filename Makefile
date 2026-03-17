DESTDIR=
BINDIR=/usr/bin

all: touch-test

touch-test: touch-test.c
	$(CC) -O3 -o touch-test touch-test.c -lgpiod -lm

clean:
	-rm touch-test

install: touch-test
	install -d $(DESTDIR)/$(BINDIR)
	install -m 755 $< $(DESTDIR)/$(BINDIR)

.PHONY: all clean install
