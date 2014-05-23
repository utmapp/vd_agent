ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}
NULL =

bin_PROGRAMS = src/spice-vdagent
sbin_PROGRAMS = src/spice-vdagentd

src_spice_vdagent_CFLAGS =			\
	$(X_CFLAGS)				\
	$(SPICE_CFLAGS)				\
	$(GLIB2_CFLAGS)				\
	$(ALSA_CFLAGS)				\
	-DUDSCS_NO_SERVER			\
	$(NULL)

src_spice_vdagent_LDADD =			\
	$(X_LIBS)				\
	$(SPICE_LIBS)				\
	$(GLIB2_LIBS)				\
	$(ALSA_LIBS)				\
	$(NULL)

src_spice_vdagent_SOURCES =			\
	src/udscs.c				\
	src/udscs.h				\
	src/vdagent-audio.c			\
	src/vdagent-audio.h			\
	src/vdagent-file-xfers.c		\
	src/vdagent-file-xfers.h		\
	src/vdagent-x11-priv.h			\
	src/vdagent-x11-randr.c			\
	src/vdagent-x11.c			\
	src/vdagent-x11.h			\
	src/vdagent.c				\
	$(NULL)

src_spice_vdagentd_CFLAGS =			\
	$(DBUS_CFLAGS)				\
	$(LIBSYSTEMD_LOGIN_CFLAGS)		\
	$(PCIACCESS_CFLAGS)			\
	$(SPICE_CFLAGS)				\
	$(GLIB2_CFLAGS)				\
	$(PIE_CFLAGS)				\
	$(NULL)

src_spice_vdagentd_LDADD =			\
	$(DBUS_LIBS)				\
	$(LIBSYSTEMD_LOGIN_LIBS)		\
	$(PCIACCESS_LIBS)			\
	$(SPICE_LIBS)				\
	$(GLIB2_LIBS)				\
	$(PIE_LDFLAGS)				\
	$(NULL)

src_spice_vdagentd_SOURCES =			\
	src/vdagentd.c				\
	src/session-info.h			\
	src/vdagentd-proto-strings.h		\
	src/vdagentd-proto.h			\
	src/vdagentd-uinput.c			\
	src/vdagentd-uinput.h			\
	src/vdagentd-xorg-conf.c		\
	src/vdagentd-xorg-conf.h		\
	src/vdagent-virtio-port.c		\
	src/vdagent-virtio-port.h		\
	src/udscs.c				\
	src/udscs.h				\
	$(NULL)

if HAVE_CONSOLE_KIT
src_spice_vdagentd_SOURCES += src/console-kit.c
else
if HAVE_LIBSYSTEMD_LOGIN
src_spice_vdagentd_SOURCES += src/systemd-login.c
else
src_spice_vdagentd_SOURCES += src/dummy-session-info.c
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
	$(top_srcdir)/data/spice-vdagentd.target

udevrulesdir = /lib/udev/rules.d
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
	NEWS					\
	README.RHEL-5				\
	data/70-spice-vdagentd.rules		\
	data/spice-vdagent.desktop		\
	data/spice-vdagentd			\
	data/spice-vdagentd.service		\
	data/spice-vdagentd.target		\
	data/tmpfiles.d/spice-vdagentd.conf	\
	data/xorg.conf.RHEL-5			\
	$(NULL)

DISTCHECK_CONFIGURE_FLAGS =			\
	--with-init-script=redhat		\
	$(NULL)
