# 将本地 fxcorr 仓库同步到 Linux 测试机（ssh 别名 fxcorr）的 /root/fxcorr
# 用法：在 fxcorr/ 目录下运行 make sync
# 排除：git 元数据、运行时数据目录、autotools 生成物与编译产物
# （生成物在测试机各自 autoreconf/configure 重新生成，避免 macOS/Linux 工具链版本错配）

REMOTE := fxcorr:/root/fxcorr/
SRC := $(abspath ..)/

RSYNC_EXCLUDES := \
	--exclude=.git/ \
	--exclude=difx-data/ \
	--exclude=config/ \
	--exclude=raw/ \
	--exclude=fengine/ \
	--exclude=vis/ \
	--exclude=product/ \
	--exclude=meta/ \
	--exclude=work/ \
	--include='fxcorr/docker/Makefile' \
	--exclude='*/Makefile' \
	--exclude='*/Makefile.in' \
	--exclude='*/configure' \
	--exclude='*/aclocal.m4' \
	--exclude='*/autom4te.cache/' \
	--exclude='*/config.h' \
	--exclude='*/config.h.in' \
	--exclude='*/config.log' \
	--exclude='*/config.status' \
	--exclude='*/config.guess' \
	--exclude='*/config.sub' \
	--exclude='*/compile' \
	--exclude='*/depcomp' \
	--exclude='*/install-sh' \
	--exclude='*/missing' \
	--exclude='*/ltmain.sh' \
	--exclude='*/libtool' \
	--exclude='*/stamp-h1' \
	--exclude='*/architecture.h' \
	--exclude='.deps/' \
	--exclude='.libs/' \
	--exclude='*.o' \
	--exclude='*.lo' \
	--exclude='*.la'

sync:
	rsync -azc --delete $(RSYNC_EXCLUDES) $(SRC) $(REMOTE)

.PHONY: sync
