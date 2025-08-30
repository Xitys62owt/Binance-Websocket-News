.PHONY: install run

install:
	@sudo apt-get update
	@sudo apt-get install -y build-essential cmake libboost-system-dev libssl-dev libssl1.1 git

run:
	@rm -rf build
	@mkdir -p build
	@cd build && cmake .. && make -j$(nproc)
	@./build/Binance-WebSocket-news
