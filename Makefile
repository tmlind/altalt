all: clean
	$(CROSS_COMPILER)gcc -Wall -o altalt altalt.c

clean:
	rm -f altalt
