
# 工具链与通用参数
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -pipe
RM       := rm -f

# 源文件（服务器固定，客户端可在 GUI/CLI 间切换）
SERVER_SRC := server.cpp
SERVER_EXE := server.exe

# --- 方式一：Win32 GUI 客户端（默认） ---
CLIENT_SRC := client.cpp
CLIENT_EXE := client.exe

# 若需要命令行版客户端，构建时加：make CLIENT_MODE=cli
ifeq ($(CLIENT_MODE),cli)
  CLIENT_SRC := client.cpp
endif

# 链接库
WIN_LIBS := -lws2_32
GUI_LIBS := -luser32 -lgdi32 -municode -mwindows

# 根据客户端模式选择库
ifeq ($(CLIENT_MODE),cli)
  CLIENT_LIBS := $(WIN_LIBS)
else
  CLIENT_LIBS := $(WIN_LIBS) $(GUI_LIBS)
endif

# ===== 目标 =====

all: $(SERVER_EXE) $(CLIENT_EXE)

$(SERVER_EXE): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) $< $(WIN_LIBS) -o $@

$(CLIENT_EXE): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) $< $(CLIENT_LIBS) -o $@

# 便捷运行
run-server: $(SERVER_EXE)
	./$(SERVER_EXE) 5000

run-client: $(CLIENT_EXE)
	./$(CLIENT_EXE)

# 清理（删除 exe 与常见中间产物）
clean:
	$(RM) $(SERVER_EXE) $(CLIENT_EXE) *.o *.obj

.PHONY: all clean run-server run-client
