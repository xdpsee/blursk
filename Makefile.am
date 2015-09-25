lib_LTLIBRARIES = libblursk.la

libdir = $(plugindir)/$(VISUALIZATION_PLUGIN_DIR)

libblursk_la_LDFLAGS = -module -avoid-version
LIBS = @GTK_LIBS@ @PTHREAD_LIBS@

COMMON_CFLAGS = -O2 -I$(top_builddir) -I$(top_srcdir) -D_REENTRANT @GTK_CFLAGS@
COMMON_SRC = blur.c blursk.c blursk.h color.c config.c config.h \
	readme.c about.c img.c loop.c render.c preset.c bitmap.c \
	paste.c xv.c text.c

if ARCH_X86

EXTRA_CFLAGS = -DX86_ASM_OPT
EXTRA_SRC = loopx86.s

else

EXTRA_SRC = 

endif

EXTRA_DIST = bitmaps/blursk.xbm bitmaps/bob.xbm bitmaps/clef.xbm \
	bitmaps/flower.xbm bitmaps/gnome.xbm bitmaps/notes.xbm \
	bitmaps/peace.xbm bitmaps/question.xbm bitmaps/redhat.xbm \
	bitmaps/suse.xbm bitmaps/tux.xbm bitmaps/xlogo.xbm bitmaps/xmms.xbm \
	logo.xpm README loopx86.s btnblack.xbm btnwhite.xbm


CFLAGS = $(COMMON_CFLAGS) $(EXTRA_CFLAGS)
libblursk_la_SOURCES =  $(COMMON_SRC) $(EXTRA_SRC)

readme.c: README
	echo '/* Automatically generated from README, via Makefile.am */' >readme.c
	echo 'char readme[] =' >>readme.c
	sed 's/"/\\"/g; s/$$/\\n/; s/.*/	"&"/' README >>readme.c
	echo ';' >>readme.c

$(distdir).tar.gz: dist
	@# no commands needed

$(distdir).rpm: Blursk.spec $(distdir).tar.gz
	@topdir=`rpm --showrc|sed -n 's/^[-0-9]*: _topdir[ 	]%{_usrsrc}/\/usr\/src/p'`; \
	 origdir=`pwd`; \
	 echo topdir=$$topdir; \
	 test -d $$topdir || topdir=/usr/src/redhat; \
	 echo topdir=$$topdir; \
	 if test -d $$topdir; \
	 then \
		 cp Blursk.spec $$topdir/SPECS/; \
		 cp $(distdir).tar.gz $$topdir/SOURCES; \
		 cd $$topdir; \
		 rpm -bb SPECS/Blursk.spec; \
	 else \
		 echo "Can't find rpm's topdir"; \
		 exit 2; \
	 fi;\
	 cp $$topdir/RPMS/i[53]86/Blursk* $$origdir/$(distdir).rpm

web: websrc $(distdir).tar.gz $(distdir).rpm .libs/libblursk.so
	rm -rf web
	mkdir web
	cp websrc/* web/
	cp $(distdir).tar.gz $(distdir).rpm .libs/libblursk.so web/
	sed "s/BLURSK_TGZ_LEN/`wc -c <web/$(distdir).tar.gz`/; \
	     s/BLURSK_RPM_LEN/`wc -c <web/$(distdir).rpm`/; \
	     s/LIBBLURSK_LEN/`wc -c <web/libblursk.so`/; \
	     s/BLURSK_PRESETS_LEN/`wc -c <web/blursk-presets`/" \
		websrc/index.html >web/index.html

