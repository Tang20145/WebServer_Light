CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) ./synchronize/synchronize.cpp ./timer/timer.cpp ./myHttp/myHttp.cpp ./mysql/mysqlPool.cpp ./myLog/myLog.cpp -lpthread -L/usr/lib64/mysql -lmysqlclient -lpthread -ldl -lssl -lcrypto -lresolv -lm -lrt

clean:
	rm  -r server
