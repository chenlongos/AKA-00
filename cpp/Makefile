# =============================================================================
# cpp/ 顶层 Makefile —— make 一键构建（libjpeg + csrc/capp 交叉编译 + 打包）
#
# 在 orb（Linux + riscv64 musl 工具链）里直接 make；在 macOS 上 make 会自动经
# `orb run -p` 转发交叉编译（产物落在共享目录，两边路径一致）。
# 前端不参与构建：直接用仓库根 static/（已构建产物）打包。
#
# 用法:
#   make                    # 全流程: libjpeg → csrc → capp → package
#   make libjpeg            # 交叉编译 libjpeg（首次自动下载源码）
#   make csrc               # 交叉编译 csrc（libcsrc.a, riscv64）
#   make capp               # 交叉编译 capp（bin/aka-capp, riscv64 静态）
#   make package            # 组装 dist/AKA-00/ + aka-capp.tar.gz
#   make all                # 同上（默认目标）
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
CSRC_LIB := $(CPP)/csrc/build-cross/libcsrc.a
CAPP_BIN := $(CPP)/capp/bin/aka-capp

.PHONY: all libjpeg csrc capp tools package clean FORCE
all: package

libjpeg: $(JPEG)
csrc: $(CSRC_LIB)
capp: $(CAPP_BIN)
tools: $(CSRC_LIB)
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross tools"

$(JPEG):
	$(ORB_RUN) "cd $(CPP)/scripts && ./build-libjpeg.sh"

$(CSRC_LIB): $(JPEG)
	$(ORB_RUN) "cd $(CPP)/csrc && make -f Makefile.cross"

# FORCE: bin/aka-capp 可能被本机 make（host 版）覆盖成非 RISC-V 二进制，
# make 只看 mtime 会误判为最新。每次都跑交叉编译（内部增量，秒级），
# 确保部署产物永远是 riscv64 静态版。
$(CAPP_BIN): $(CSRC_LIB) FORCE
	$(ORB_RUN) "cd $(CPP)/capp && make -f Makefile.cross"

# ── 打包：组装 dist/AKA-00/ + aka-capp.tar.gz ──
package: $(CAPP_BIN) tools
	@set -e; \
	DIST="$(CPP)/dist/AKA-00"; \
	rm -rf "$$DIST"; mkdir -p "$$DIST/demo"; \
	echo "── 组装部署目录 → $$DIST ──"; \
	cp "$(CAPP_BIN)" "$$DIST/aka-capp"; chmod 755 "$$DIST/aka-capp"; \
	if [ -f "$(ROOT)/static/index.html" ]; then cp -r "$(ROOT)/static" "$$DIST/static"; fi; \
	[ -f "$(ROOT)/config.toml" ] && cp "$(ROOT)/config.toml" "$$DIST/config.toml"; \
	[ -f "$(ROOT)/arm_angles.json" ] && cp "$(ROOT)/arm_angles.json" "$$DIST/"; \
	[ -f "$(ROOT)/arm_angles_default.json" ] && cp "$(ROOT)/arm_angles_default.json" "$$DIST/"; \
	[ -f "$(ROOT)/speed_config.json" ] && cp "$(ROOT)/speed_config.json" "$$DIST/"; \
	[ -f "$(ROOT)/VERSION" ] && cp "$(ROOT)/VERSION" "$$DIST/"; \
	if [ ! -f "$$DIST/config.toml" ]; then \
	    printf '%s\n' \
	        '[camera]' 'width = 320' 'height = 240' 'fps = 24' 'jpeg_quality = 30' \
	        '' '[motor]' 'backend = "tt_pid"' 'port = "/dev/ttyS1"' 'baudrate = 115200' 'ppr = 4680' \
	        '' '[arm]' 'backend = "zp10s"' 'port = "/dev/ttyS2"' 'baudrate = 115200' \
	        '' '[web]' 'port = 80' \
	        '' '[ota]' 'check_url = "https://api.chenlongrobot.com/api/user/robot-versions/featured"' \
	        '' '[chassis]' 'wheel_diameter_mm = 62' 'gear_ratio = 90' \
	        '' '[logging]' 'level = "info"' \
	        > "$$DIST/config.toml"; \
	    echo "  （无根 config.toml，已生成默认配置）"; \
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
	chmod 755 "$$DIST/init.sh" "$$DIST/stop.sh" "$$DIST/init_ap_web.sh"; \
	if [ -f "$(CPP)/csrc/build-cross/tt_pid_test" ]; then \
	    mkdir -p "$$DIST/tools"; \
	    cp "$(CPP)/csrc/build-cross/tt_pid_test" "$$DIST/tools/"; \
	    chmod 755 "$$DIST/tools/tt_pid_test"; \
	    cp "$(CPP)/csrc/tools/ws_joystick_test.py" "$$DIST/tools/"; \
	fi; \
	echo ""; echo "  ✓ 部署目录: $$DIST"; du -sh "$$DIST" 2>/dev/null || true; \
	echo ""; echo "  板子上: scp dist/aka-capp.tar.gz root@<板子IP>:/root/"; \
	echo "          ssh root@<板子IP> \"cd /root && tar -xzf aka-capp.tar.gz\""; \
	echo "          → 解压落到 /root/AKA-00/，然后 AKA_HOME=/root/AKA-00 /root/AKA-00/init.sh"; \
	cd "$(CPP)/dist" && tar -czf aka-capp.tar.gz AKA-00; \
	echo "  ✓ 打包: $(CPP)/dist/aka-capp.tar.gz（顶层 AKA-00/，解压落到 /root/AKA-00/）"

clean:
	$(ORB_RUN) "rm -rf $(CPP)/csrc/build-cross $(CPP)/capp/build-cross $(CPP)/capp/bin $(CPP)/dist $(CPP)/third_party"
