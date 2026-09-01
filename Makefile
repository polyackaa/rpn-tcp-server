CC = gcc
CFLAGS = -Wall -Wextra -Werror -O3 -fanalyzer
SERVER = lab3pnsN3246_server
CLIENT = lab3pnsN3246_client

.PHONY: all clean test valgrind 

all: $(SERVER) $(CLIENT)

$(SERVER): lab3pnsN3246_server.c common.h
	$(CC) $(CFLAGS) lab3pnsN3246_server.c -o $(SERVER)

$(CLIENT): lab3pnsN3246_client.c common.h
	$(CC) $(CFLAGS) lab3pnsN3246_client.c -o $(CLIENT)

test: all
	@echo "=== Запуск сервера ==="
	./$(SERVER) -a 127.0.0.1 -p 5555 -l ./lab3_test.log & echo $$! > server.pid
	@sleep 1
	@echo "=== Тест 1 ==="
	./$(CLIENT) -a 127.0.0.1 -p 5555 "2 + 3 * 4"
	@echo "=== Тест 2 ==="
	./$(CLIENT) -a 127.0.0.1 -p 5555 "( 2 + 3 ) * 4"
	@echo "=== Тест 3 ==="
	./$(CLIENT) -a 127.0.0.1 -p 5555 "10 / ( 2 + 3 )"
	@echo "=== Тест 4: ошибка ==="
	./$(CLIENT) -a 127.0.0.1 -p 5555 "2 + * 3"
	@echo "=== Тест 5: одновременные запросы ==="
	./$(CLIENT) -a 127.0.0.1 -p 5555 "1 + 2" & \
	./$(CLIENT) -a 127.0.0.1 -p 5555 "3 * 4" & \
	./$(CLIENT) -a 127.0.0.1 -p 5555 "5 - 6 / 2" & wait
	@kill -TERM `cat server.pid`
	@rm -f server.pid

valgrind: all
	valgrind --leak-check=full --show-leak-kinds=all ./$(CLIENT) -a 127.0.0.1 -p 5555 "2 + 3 * 4" > valgrind.txt 2>&1 || true

clean:
	rm -f $(SERVER) $(CLIENT) server.pid lab3_test.log 
