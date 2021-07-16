ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}
NULL =

bin_PROGRAMS = src/spice-vdagent
sbin_PROGRAMS = src/spice-vdagentd
check_PROGRAMS = tests/test-file-xfers
TESTS = $(check_PROGRAMS)

common_sources =				\
	src/udscs.c				\
	src/udscs.h				\
	src/vdagent-connection.c		\
	src/vdagent-connection.h		\
	src/vdagentd-proto-strings.h		\
	src/vdagentd-proto.h			\
	$(NULL)

src_spice_vdagent_CFLAGS =			\
	$(DRM_CFLAGS)				\
	$(X_CFLAGS)				\
	$(SPICE_CFLAGS)				\
	$(GIO2_CFLAGS)				\
	$(GTK_CFLAGS)				\
	$(ALSA_CFLAGS)				\
	-I$(srcdir)/src				\
	-DUDSCS_NO_SERVER			\
	$(NULL)

src_spice_vdagent_LDADD =			\
	$(DRM_LIBS)				\
	$(X_LIBS)				\
	$(SPICE_LIBS)				\
	$(GIO2_LIBS)				\
	$(GTK_LIBS)				\
	$(ALSA_LIBS)				\
	$(NULL)

src_spice_vdagent_SOURCES =			\
	$(common_sources)			\
	src/vdagent/audio.c			\
	src/vdagent/audio.h			\
	src/vdagent/clipboard.c			\
	src/vdagent/clipboard.h			\
	src/vdagent/webdav-cb.c			\
	src/vdagent/webdav-cb.h			\
	src/vdagent/device-info.c		\
	src/vdagent/device-info.h		\
	src/vdagent/display.c			\
	src/vdagent/display.h			\
	src/vdagent/file-xfers.c		\
	src/vdagent/file-xfers.h		\
	src/vdagent/mutter.c			\
	src/vdagent/mutter.h			\
	src/vdagent/x11-priv.h			\
	src/vdagent/x11-randr.c			\
	src/vdagent/x11.c			\
	src/vdagent/x11.h			\
	src/vdagent/vdagent.c			\
	$(NULL)

tests_test_file_xfers_CFLAGS =			\
	$(SPICE_CFLAGS)				\
	$(GIO2_CFLAGS)				\
	-I$(srcdir)/src				\
	-I$(srcdir)/src/vdagent			\
	-DUDSCS_NO_SERVER			\
	$(NULL)

tests_test_file_xfers_LDADD =			\
	$(SPICE_LIBS)				\
	$(GIO2_LIBS)				\
	$(NULL)

tests_test_file_xfers_SOURCES =			\
	$(common_sources)			\
	src/vdagent/file-xfers.c		\
	src/vdagent/file-xfers.h		\
	tests/test-file-xfers.c			\
	$(NULL)

src_spice_vdagentd_CFLAGS =			\
	$(DBUS_CFLAGS)				\
	$(LIBSYSTEMD_DAEMON_CFLAGS)		\
	$(LIBSYSTEMD_LOGIN_CFLAGS)		\
	$(PCIACCESS_CFLAGS)			\
	$(SPICE_CFLAGS)				\
	$(GIO2_CFLAGS)				\
	$(PIE_CFLAGS)				\
	-I$(srcdir)/src				\
	$(NULL)

src_spice_vdagentd_LDADD =			\
	$(DBUS_LIBS)				\
	$(LIBSYSTEMD_DAEMON_LIBS)		\
	$(LIBSYSTEMD_LOGIN_LIBS)		\
	$(PCIACCESS_LIBS)			\
	$(SPICE_LIBS)				\
	$(GIO2_LIBS)				\
	$(PIE_LDFLAGS)				\
	$(NULL)

src_spice_vdagentd_SOURCES =			\
	$(common_sources)			\
	src/vdagentd/vdagentd.c			\
	src/vdagentd/session-info.h		\
	src/vdagentd/uinput.c			\
	src/vdagentd/uinput.h			\
	src/vdagentd/xorg-conf.c		\
	src/vdagentd/xorg-conf.h		\
	src/vdagentd/virtio-port.c		\
	src/vdagentd/virtio-port.h		\
	$(NULL)

tests_test_session_info_CFLAGS =		\
	$(DBUS_CFLAGS)				\
	$(GIO2_CFLAGS)				\
	-I$(srcdir)/src				\
	-I$(srcdir)/src/vdagentd		\
	$(NULL)

tests_test_session_info_LDADD =			\
	$(DBUS_LIBS)				\
	$(GIO2_LIBS)				\
	$(NULL)

tests_test_session_info_SOURCES =		\
	src/vdagentd/session-info.h		\
	tests/test-session-info.c		\
	$(NULL)

if HAVE_CONSOLE_KIT
src_spice_vdagentd_SOURCES += src/vdagentd/console-kit.c
tests_test_session_info_SOURCES += src/vdagentd/console-kit.c
check_PROGRAMS += tests/test-session-info
else
if HAVE_LIBSYSTEMD_LOGIN
src_spice_vdagentd_SOURCES += src/vdagentd/systemd-login.c
tests_test_session_info_CFLAGS += $(LIBSYSTEMD_LOGIN_CFLAGS)
tests_test_session_info_LDADD += $(LIBSYSTEMD_LOGIN_LIBS)
tests_test_session_info_SOURCES += src/vdagentd/systemd-login.c
check_PROGRAMS += tests/test-session-info
else
src_spice_vdagentd_SOURCES += src/vdagentd/dummy-session-info.c
endif
endif

xdgautostartdir = $(sysconfdir)/xdg/autostart
xdgautostart_DATA = $(top_srcdir)/data/spice-vdagent.desktop

gdmautostartdir = $(datadir)/gdm/greeter/autostart
gdmautostart_DATA = $(top_srcdir)/data/spice-vdagent.desktop

gdmautostart2dir = $(datadir)/gdm/autostart/LoginWindow
gdmautostart2_DATA = $(top_srcdir)/data/spice-vdagent.desktop

install-data-local:
	$(mkdir_p) $(DESTDIR)$(localstatedir)/run/spice-vdagentd

if INIT_SCRIPT_RED_HAT
initdir = $(sysconfdir)/rc.d/init.d
init_SCRIPTS = $(top_srcdir)/data/spice-vdagentd
endif

if INIT_SCRIPT_SYSTEMD
systemdunitdir = $(SYSTEMDSYSTEMUNITDIR)
systemdunit_DATA = \
	$(top_srcdir)/data/spice-vdagentd.service \
	$(top_srcdir)/data/spice-vdagentd.socket

userunitdir = $(SYSTEMDUSERUNITDIR)
userunit_DATA = \
	$(top_srcdir)/data/spice-vdagent.service

udevrulesdir = $(udevdir)/rules.d
udevrules_DATA = $(top_srcdir)/data/70-spice-vdagentd.rules

tmpfilesdir = $(prefix)/lib/tmpfiles.d
tmpfiles_DATA = $(top_srcdir)/data/tmpfiles.d/spice-vdagentd.conf
endif

manpagedir = $(mandir)/man1
manpage_DATA =					\
	data/spice-vdagent.1			\
	data/spice-vdagentd.1			\
	$(NULL)

EXTRA_DIST =					\
	README.md				\
	CHANGELOG.md				\
	data/70-spice-vdagentd.rules		\
	data/spice-vdagent.desktop		\
	data/spice-vdagent.service	   	\
	data/spice-vdagentd			\
	data/spice-vdagentd.service		\
	data/spice-vdagentd.socket		\
	data/tmpfiles.d/spice-vdagentd.conf	\
	data/xorg.conf.RHEL-5			\
	$(NULL)

DISTCHECK_CONFIGURE_FLAGS =			\
	--with-init-script=redhat		\
	$(NULL)

tests_test_device_info_LDADD = $(src_spice_vdagent_LDADD)
tests_test_device_info_CFLAGS = $(src_spice_vdagent_CFLAGS)

check_PROGRAMS += tests/test-device-info

check_PROGRAMS += tests/test-termination
