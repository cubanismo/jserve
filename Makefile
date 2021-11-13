
jserve: jserve.cpp
	gcc -g -O0 -o jserve jserve.cpp -lusb

.PHONY: clean
clean:
	rm -f jserve *.o
