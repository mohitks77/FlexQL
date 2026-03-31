CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  :=

OBJS := build/network.o build/storage.o build/parser.o build/cache.o build/executor.o

.PHONY: all clean dirs

all: dirs bin/flexql-server bin/flexql-client
	@echo ""
	@echo "  Build complete!"
	@echo "  Start server : ./bin/flexql-server 9000"
	@echo "  Connect      : ./bin/flexql-client 127.0.0.1 9000"
	@echo "  Benchmark    : python3 scripts/benchmark.py 127.0.0.1 9000 1000000"
	@echo "  Cache compare: python3 scripts/compare_cache.py 100000"

dirs:
	@mkdir -p bin build data

build/network.o:  src/network/network.cpp  include/network.h  include/common.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/storage.o:  src/storage/storage.cpp  include/storage.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/parser.o:   src/parser/parser.cpp    include/parser.h   include/storage.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/cache.o:    src/cache/cache.cpp      include/cache.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/executor.o: src/query/executor.cpp   include/executor.h include/cache.h include/parser.h include/storage.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/flexql_client.o: src/client/flexql_client.cpp include/flexql.h include/network.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/network_client.o: src/network/network.cpp include/network.h include/common.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

bin/flexql-server: src/server/server.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[OK] bin/flexql-server"

bin/flexql-client: src/client/repl.cpp build/flexql_client.o build/network_client.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[OK] bin/flexql-client"

clean:
	rm -rf bin build
	@echo "[OK] Cleaned"

bin/cbench: scripts/cbench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] bin/cbench"
