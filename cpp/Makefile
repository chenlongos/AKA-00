# =============================================================================
# cpp/ 顶层 Makefile —— make 一键构建（libjpeg + csrc/capp 交叉编译 + 打包）
#
# 在 orb（Linux + riscv64 musl 工具链）里直接 make；在 macOS 上 make 会自动经
# `orb run -p` 转发交叉编译（产物落在共享目录，两边路径一致）。
# 前端不参与构建：直接用仓库根 static/（已构建产物）打包。
#
# 打包（package）只做三件事：把 cpp/board/（= 板上目录 $AKA_HOME/ 的实体文件镜像）
# 原样收进 dist/AKA-00/，补三样构建产物（capp 二进制 / static / tools），
# 不带屏版再删掉 start_img.jpg。**板上要什么就改 cpp/board/ 里的实体文件**，
# 这里不再有逐文件的拷贝清单。
#
# ── 两个版本（编译期开关 AKA_WITH_SCREEN，构建目录/产物完全隔离，互不覆盖）──
#   make / make screen     带屏版本：bin/aka-capp + dist/AKA-00/（+ make ota 时再出安装器）
#   make noscreen          不带屏版本：bin/aka-capp-noscreen + dist-noscreen/AKA-00/
#   （不带屏版本把整个显示栈（/dev/fb0）编译期裁掉：二进制更小、完全不碰 framebuffer，
#     适合没有屏的机器；[display] 配置项在该版本中被忽略）
#
# 用法:
#   make                    # 带屏版全流程: libjpeg → csrc → capp → package
#   make noscreen           # 不带屏版全流程
#   make libjpeg            # 交叉编译 libjpeg（首次自动下载源码）
#   make csrc | capp | tools | package                 # 带屏版各步骤
#   make csrc-noscreen | capp-noscreen | package-noscreen   # 不带屏版各步骤
#   make all                # 同 make（默认目标 = 带屏版）
#   make clean
# =============================================================================

# ── 执行环境：orb 里本地跑，macOS 上经 orb run 转发 ──
ORB_RUN := bash -lc
ifeq ($(shell uname -s),Darwin)
ORB_RUN := orb run -p bash -lc
endif

# ── 产物路径（orb 与 macOS 共享文件系统，路径一致）──
CPP      := $(CURDIR)
ROOT     := $(CPP)/..
JPEG     := $(CPP)/third_party/jpeg/lib/libjpeg.a
MBEDTLS  := $(CPP)/third_party/mbedtls/lib/libmbedtls.a
LUA      := $(CPP)/third_party/lua/lib/liblua.a

# 带屏版
CSRC_LIB     := $(CPP)/csrc/build-cross/libcsrc.a
CAPP_BIN     := $(CPP)/capp/bin/aka-capp
DIST         := $(CPP)/dist/AKA-00
OBJDIR       := build-cross

# 不带屏版
CSRC_LIB_NS  := $(CPP)/csrc/build-cross-noscreen/libcsrc.a
CAPP_BIN_NS  := $(CPP)/capp/bin/aka-capp-noscreen
DIST_NS      := $(CPP)/dist-noscreen/AKA-00
OBJDIR_NS    := build-cross-noscreen

.PHONY: all screen noscreen libjpeg mbedtls lua csrc capp tools package ota \
        csrc-noscreen capp-noscreen tools-noscreen package-noscreen demos clean FORCE

# 默认：只产出两样东西 —— dist/AKA-00/（部署目录）与 dist/aka-00-server（自解压安装器）
all: package ota
screen: package
ota: package
	@$(ORB_RUN) "cd $(CPP) && sh scripts/build-ota.sh"
noscreen: package-noscreen

libjpeg: $(JPEG)
mbedtls: $(MBEDTLS)
lua: $(LUA)

csrc: $(CSRC_LIB)
capp: $(CAPP_BIN)
tools: $(CSRC_LIB)
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=1 tools"

csrc-noscreen: $(CSRC_LIB_NS)
capp-noscreen: $(CAPP_BIN_NS)
tools-noscreen: $(CSRC_LIB_NS)
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=0 tools"

# 独立板测 demo（tests/ 下的单文件 C 程序：摄像头 → 屏 闭环；与 capp 无关，
# 现场排障用：DEMO_TEST=1 验颜色/方向、DEMO_BENCH=1 验写屏带宽、裸跑看 fps/耗时）。
# 注意：demo 独占摄像头，跑之前先停 capp。
DEMO_DIR := $(CPP)/csrc/build-cross/demos
DEMO_BINS := $(DEMO_DIR)/demo_camera $(DEMO_DIR)/demo_image

demos: $(DEMO_BINS)

$(DEMO_BINS): $(ROOT)/tests/demo_camera.c $(ROOT)/tests/demo_image.c $(JPEG)
	$(ORB_RUN) "set -e; export PATH=/home/junbo_dai/riscv64-linux-musl-x86_64/bin:\$$PATH; \
	    mkdir -p $(DEMO_DIR); \
	    riscv64-unknown-linux-musl-gcc -O2 -Wall -static -o $(DEMO_DIR)/demo_camera \
	        $(ROOT)/tests/demo_camera.c -I$(CPP)/third_party/jpeg/include $(JPEG); \
	    riscv64-unknown-linux-musl-gcc -O2 -Wall -static -o $(DEMO_DIR)/demo_image \
	        $(ROOT)/tests/demo_image.c -I$(CPP)/third_party/jpeg/include $(JPEG); \
	    echo '  ✓ 板测 demo: demo_camera / demo_image'"

$(JPEG):
	$(ORB_RUN) "cd $(CPP)/scripts && ./build-libjpeg.sh"

$(MBEDTLS):
	$(ORB_RUN) "cd $(CPP)/scripts && ./build-mbedtls.sh"

# Lua：流程脚本宿主（scripts/*.lua）。纯 C、无依赖，编成静态库链进 capp。
$(LUA):
	$(ORB_RUN) "cd $(CPP)/scripts && ./build-lua.sh"

# FORCE 同 capp：csrc 源/头文件改动时保证重编（内部增量，秒级）
$(CSRC_LIB): $(JPEG) FORCE
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=1"

$(CSRC_LIB_NS): $(JPEG) FORCE
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=0"

# FORCE: bin/aka-capp* 可能被本机 make（host 版）覆盖成非 RISC-V 二进制，
# make 只看 mtime 会误判为最新。每次都跑交叉编译（内部增量，秒级），
# 确保部署产物永远是 riscv64 静态版。
$(CAPP_BIN): $(CSRC_LIB) $(MBEDTLS) $(LUA) FORCE
	$(ORB_RUN) "cd $(CPP)/capp && make -f Makefile.cross WITH_SCREEN=1"

$(CAPP_BIN_NS): $(CSRC_LIB_NS) $(MBEDTLS) $(LUA) FORCE
	$(ORB_RUN) "cd $(CPP)/capp && make -f Makefile.cross WITH_SCREEN=0"

# ── 打包 ──
# 板上要什么是**实体文件**说了算：`cpp/board/` 就是 `$AKA_HOME/` 的镜像，
# 想改 init.sh、config.toml、demo/*.lua 就直接改那里的文件，不用碰构建脚本。
# 这里只做三件事：① 把 board/ 原样收过来 ② 补三样构建产物（aka-capp / static / tools）
# ③ 不带屏版把 start_img.jpg 拿掉（显示栈整个不存在，留着没意义）。
#
# 参数：$(1)=带屏标志(1/0)  $(2)=capp 二进制  $(3)=部署目录  $(4)=csrc 构建目录
#       $(5)=部署内的二进制名（不带屏版叫 aka-capp-noscreen）
define PACKAGE_RECIPE
	@set -e; \
	DIST="$(3)"; \
	rm -rf "$$DIST"; \
	cp -r "$(CPP)/board" "$$DIST"; \
	cp "$(2)" "$$DIST/$(5)"; \
	chmod 755 "$$DIST/$(5)" "$$DIST"/*.sh; \
	[ -f "$(ROOT)/static/index.html" ] && cp -r "$(ROOT)/static" "$$DIST/static"; \
	if [ "$(1)" = "0" ]; then rm -f "$$DIST/start_img.jpg"; fi; \
	mkdir -p "$$DIST/tools"; \
	for t in tt_pid_test cam_probe screen_test; do \
	    [ -f "$(CPP)/csrc/$(4)/$$t" ] && cp "$(CPP)/csrc/$(4)/$$t" "$$DIST/tools/" || true; \
	done; \
	[ -f "$(CPP)/csrc/tools/ws_joystick_test.py" ] && cp "$(CPP)/csrc/tools/ws_joystick_test.py" "$$DIST/tools/" || true; \
	[ -f "$(DEMO_DIR)/demo_camera" ] && cp "$(DEMO_DIR)/demo_camera" "$(DEMO_DIR)/demo_image" "$$DIST/tools/" || true; \
	chmod 755 "$$DIST/tools"/* 2>/dev/null || true; \
	echo "── 部署目录 $${DIST}（$(if $(filter 1,$(1)),带屏,不带屏)）──"; \
	echo "   demo/：$$(ls "$$DIST/demo"/*.lua 2>/dev/null | wc -l | tr -d ' ') 个脚本、$$(find "$$DIST/demo/models" -name '*.cvimodel' 2>/dev/null | wc -l | tr -d ' ') 个模型、$$(ls "$$DIST/demo/configs"/*.json 2>/dev/null | wc -l | tr -d ' ') 份参数"; \
	du -sh "$$DIST"; \
	echo ""; \
	echo "  部署到板子：make ota 生成 dist/aka-00-server，scp 过去后执行"; \
	echo "      ./aka-00-server --update                          # 升级（保 config.toml 等现场文件）"; \
	echo "      AKA_OTA_RESET_CONFIG=1 ./aka-00-server --update   # 连 config.toml 一起换新"
endef

# ── 打包：带屏版 → dist/AKA-00/（部署目录）──
package: $(CAPP_BIN) tools demos
	$(call PACKAGE_RECIPE,1,$(CAPP_BIN),$(DIST),$(OBJDIR),aka-capp)

# ── 打包：不带屏版 → dist-noscreen/AKA-00/ ──
package-noscreen: $(CAPP_BIN_NS) tools-noscreen demos
	$(call PACKAGE_RECIPE,0,$(CAPP_BIN_NS),$(DIST_NS),$(OBJDIR_NS),aka-capp-noscreen)

clean:
	$(ORB_RUN) "rm -rf $(CPP)/csrc/build-cross $(CPP)/csrc/build-cross-noscreen \
	    $(CPP)/capp/build-cross $(CPP)/capp/build-cross-noscreen \
	    $(CPP)/capp/bin $(CPP)/dist $(CPP)/dist-noscreen $(CPP)/third_party"
