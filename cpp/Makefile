# =============================================================================
# cpp/ 顶层 Makefile —— make 一键构建（libjpeg + csrc/capp 交叉编译 + 打包）
#
# 在 orb（Linux + riscv64 musl 工具链）里直接 make；在 macOS 上 make 会自动经
# `orb run -p` 转发交叉编译（产物落在共享目录，两边路径一致）。
# 前端不参与构建：直接用仓库根 static/（已构建产物）打包。
#
# ── 两个版本（编译期开关 AKA_WITH_SCREEN，构建目录/产物完全隔离，互不覆盖）──
#   make / make screen     带屏版本：bin/aka-capp + dist/AKA-00/ + dist/aka-capp.tar.gz
#   make noscreen          不带屏版本：bin/aka-capp-noscreen + dist-noscreen/ +
#                                      dist/aka-capp-noscreen.tar.gz
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

# 带屏版
CSRC_LIB     := $(CPP)/csrc/build-cross/libcsrc.a
CAPP_BIN     := $(CPP)/capp/bin/aka-capp
DIST         := $(CPP)/dist/AKA-00
TARBALL      := aka-capp.tar.gz
OBJDIR       := build-cross

# 不带屏版
CSRC_LIB_NS  := $(CPP)/csrc/build-cross-noscreen/libcsrc.a
CAPP_BIN_NS  := $(CPP)/capp/bin/aka-capp-noscreen
DIST_NS      := $(CPP)/dist-noscreen/AKA-00
TARBALL_NS   := aka-capp-noscreen.tar.gz
OBJDIR_NS    := build-cross-noscreen

.PHONY: all screen noscreen libjpeg mbedtls csrc capp tools package \
        csrc-noscreen capp-noscreen tools-noscreen package-noscreen demos clean FORCE

all: screen
screen: package
noscreen: package-noscreen

libjpeg: $(JPEG)
mbedtls: $(MBEDTLS)

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

# FORCE 同 capp：csrc 源/头文件改动时保证重编（内部增量，秒级）
$(CSRC_LIB): $(JPEG) FORCE
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=1"

$(CSRC_LIB_NS): $(JPEG) FORCE
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross WITH_SCREEN=0"

# FORCE: bin/aka-capp* 可能被本机 make（host 版）覆盖成非 RISC-V 二进制，
# make 只看 mtime 会误判为最新。每次都跑交叉编译（内部增量，秒级），
# 确保部署产物永远是 riscv64 静态版。
$(CAPP_BIN): $(CSRC_LIB) $(MBEDTLS) FORCE
	$(ORB_RUN) "cd $(CPP)/capp && make -f Makefile.cross WITH_SCREEN=1"

$(CAPP_BIN_NS): $(CSRC_LIB_NS) $(MBEDTLS) FORCE
	$(ORB_RUN) "cd $(CPP)/capp && make -f Makefile.cross WITH_SCREEN=0"

# ── 打包宏：$(1)=带屏标志(1/0) $(2)=capp 二进制 $(3)=部署目录 $(4)=工具构建目录
#            $(5)=部署内二进制名 $(6)=tar 包名 ──
define PACKAGE_RECIPE
	@set -e; \
	DIST="$(3)"; \
	rm -rf "$$DIST"; mkdir -p "$$DIST/demo"; \
	echo "── 组装部署目录 → $$DIST （$(if $(filter 1,$(1)),带屏,不带屏) 版本）──"; \
	cp "$(2)" "$$DIST/$(5)"; chmod 755 "$$DIST/$(5)"; \
	if [ -f "$(ROOT)/static/index.html" ]; then cp -r "$(ROOT)/static" "$$DIST/static"; fi; \
	[ -f "$(CPP)/config.toml" ] && cp "$(CPP)/config.toml" "$$DIST/config.toml"; \
	[ -f "$(ROOT)/arm_angles.json" ] && cp "$(ROOT)/arm_angles.json" "$$DIST/"; \
	[ -f "$(ROOT)/arm_angles_default.json" ] && cp "$(ROOT)/arm_angles_default.json" "$$DIST/"; \
	[ -f "$(ROOT)/speed_config.json" ] && cp "$(ROOT)/speed_config.json" "$$DIST/"; \
	[ -f "$(ROOT)/VERSION" ] && cp "$(ROOT)/VERSION" "$$DIST/"; \
	if [ "$(1)" = "1" ] && [ -f "$(ROOT)/start_img.jpg" ]; then \
	    cp "$(ROOT)/start_img.jpg" "$$DIST/"; \
	    echo "  ✓ 熄屏待机图 start_img.jpg 已打包"; \
	fi; \
	if [ ! -f "$$DIST/config.toml" ]; then \
	    echo "ERROR: 缺少配置源 $(CPP)/config.toml —— config.toml 必须由 cpp/ 提供（打包不再内置生成）" >&2; \
	    exit 1; \
	fi; \
	if [ -d "$(ROOT)/demo" ]; then \
	    for d in "$(ROOT)"/demo/*/; do [ -f "$$d/init.sh" ] && cp -r "$${d%/}" "$$DIST/demo/"; done; \
	fi; \
	if [ -d "$(ROOT)/libs" ] && [ -n "$$(ls "$(ROOT)"/libs/*.so* 2>/dev/null)" ]; then \
	    mkdir -p "$$DIST/libs"; \
	    cp "$(ROOT)"/libs/*.so* "$$DIST/libs/"; \
	    echo "  ✓ libs/ 已打包（demo 运行时依赖）"; \
	fi; \
	cp "$(CPP)/capp/scripts/init.sh" "$$DIST/init.sh"; \
	cp "$(CPP)/capp/scripts/stop.sh" "$$DIST/stop.sh"; \
	cp "$(CPP)/capp/scripts/init_ap_web.sh" "$$DIST/init_ap_web.sh"; \
	if [ -f "$(ROOT)/https_init.sh" ]; then \
	    cp "$(ROOT)/https_init.sh" "$$DIST/https_init.sh"; \
	    chmod 755 "$$DIST/https_init.sh"; \
	fi; \
	chmod 755 "$$DIST/init.sh" "$$DIST/stop.sh" "$$DIST/init_ap_web.sh"; \
	if [ -f "$(CPP)/csrc/$(4)/tt_pid_test" ]; then \
	    mkdir -p "$$DIST/tools"; \
	    cp "$(CPP)/csrc/$(4)/tt_pid_test" "$$DIST/tools/"; \
	    chmod 755 "$$DIST/tools/tt_pid_test"; \
	    cp "$(CPP)/csrc/tools/ws_joystick_test.py" "$$DIST/tools/"; \
	    if [ -f "$(CPP)/csrc/$(4)/cam_probe" ]; then \
	        cp "$(CPP)/csrc/$(4)/cam_probe" "$$DIST/tools/"; \
	        chmod 755 "$$DIST/tools/cam_probe"; \
	    fi; \
	    if [ "$(1)" = "1" ] && [ -f "$(CPP)/csrc/$(4)/screen_test" ]; then \
	        cp "$(CPP)/csrc/$(4)/screen_test" "$$DIST/tools/"; \
	        chmod 755 "$$DIST/tools/screen_test"; \
	    fi; \
	    if [ -f "$(DEMO_DIR)/demo_camera" ]; then \
	        cp "$(DEMO_DIR)/demo_camera" "$(DEMO_DIR)/demo_image" "$$DIST/tools/"; \
	        chmod 755 "$$DIST/tools/demo_camera" "$$DIST/tools/demo_image"; \
	    fi; \
	fi; \
	echo ""; echo "  ✓ 部署目录: $$DIST"; du -sh "$$DIST" 2>/dev/null || true; \
	echo ""; \
	echo "  板子上: scp $$(basename $$(dirname "$$DIST"))/$(6) root@<板子IP>:/root/"; \
	echo "          ssh root@<板子IP> \"cd /root && tar -xzf $(6)\""; \
	echo "          → 解压落到 /root/AKA-00/，然后 AKA_HOME=/root/AKA-00 /root/AKA-00/init.sh"; \
	cd "$$(dirname "$$DIST")" && tar -czf "$(6)" AKA-00; \
	echo "  ✓ 打包: $$(dirname "$$DIST")/$(6)（顶层 AKA-00/，解压落到 /root/AKA-00/）"
endef

# ── 打包：带屏版 → dist/AKA-00/ + dist/aka-capp.tar.gz ──
package: $(CAPP_BIN) tools demos
	$(call PACKAGE_RECIPE,1,$(CAPP_BIN),$(DIST),$(OBJDIR),aka-capp,$(TARBALL))

# ── 打包：不带屏版 → dist-noscreen/AKA-00/ + dist-noscreen/aka-capp-noscreen.tar.gz ──
package-noscreen: $(CAPP_BIN_NS) tools-noscreen demos
	$(call PACKAGE_RECIPE,0,$(CAPP_BIN_NS),$(DIST_NS),$(OBJDIR_NS),aka-capp-noscreen,$(TARBALL_NS))

clean:
	$(ORB_RUN) "rm -rf $(CPP)/csrc/build-cross $(CPP)/csrc/build-cross-noscreen \
	    $(CPP)/capp/build-cross $(CPP)/capp/build-cross-noscreen \
	    $(CPP)/capp/bin $(CPP)/dist $(CPP)/dist-noscreen $(CPP)/third_party"
