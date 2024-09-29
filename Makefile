CPPC = g++
CPPFLAGS = -Wall -Wextra -O2 -std=c++23
LDFLAGS = 

all: kierki-serwer kierki-klient

kierki-serwer: kierki-serwer.o err.o common.o
	$(CPPC) $(CPPFLAGS) -o $@ $^ $(LDFLAGS)

kierki-klient: kierki-klient.o err.o common.o
	$(CPPC) $(CPPFLAGS) -o $@ $^ $(LDFLAGS)

kierki-serwer.o: kierki-serwer.cpp 
	$(CPPC) $(CPPFLAGS) -c -o $@ $<

kierki-klient.o: kierki-klient.cpp
	$(CPPC) $(CPPFLAGS) -c -o $@ $<

err.o: err.cpp err.h
	$(CPPC) $(CPPFLAGS) -c -o $@ $<

common.o: common.cpp common.h err.h
	$(CPPC) $(CPPFLAGS) -c -o $@ $<


clean:
	rm -f *.o kierki-serwer kierki-klient