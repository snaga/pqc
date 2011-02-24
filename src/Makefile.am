bin_PROGRAMS = pqcd

pqcd_SOURCES = pool.h version.h pqcd.conf.sample \
	pgpool.spec \
	main.c child.c pool_auth.c pool_config.l pool_error.c \
	pool_process_query.c pool_stream.c pool_connection_pool.c pool_params.c \
	pool_signal.h pool_signal.c ps_status.c strlcpy.c \
	pool_hba.c pool_list.h pool_list.c pool_path.h pool_path.c \
	pqc.c pqc.h \
	pool_type.h pool_ip.h pool_ip.c pqcd_hba.conf.sample pgpool.pam

pqcd_LDADD = -lmemcached -levent

DEFS = @DEFS@ \
	-DDEFAULT_CONFIGDIR=\"$(sysconfdir)\"

sysconf_DATA = pqcd.conf.sample pqcd_hba.conf.sample

pkgdata_DATA = pgpool.pam

AM_CPPFLAGS = -Wall -Wmissing-prototypes -Wmissing-declarations -D_GNU_SOURCE

man_MANS = pgpool.8

pgpool.8: pgpool.8.in
	sed 's,@sysconfdir\@,$(sysconfdir),g' $? >$@

CLEANFILES = pgpool.8
EXTRA_DIST = pgpool.8.in
