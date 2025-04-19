# Makefile for base package manager
# copyright (c) mkfs (Maksim Yarovoy) 2025
CXX = g++
CXXFLAGS = -std=c++17 -Wall -I/usr/include
LDFLAGS = -lcurl -lssl -lcrypto -pthread
TARGET = base
SRC = main.cpp

PREFIX = /usr
BINDIR = $(PREFIX)/bin

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

