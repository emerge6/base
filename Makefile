CXX = g++
CXXFLAGS = -std=c++17 -Wall
LIBS = -lcurl -lssl -lcrypto -pthread
TARGET = base
SOURCES = main.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)
