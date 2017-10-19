AC_PREREQ(2.59)
AC_INIT([spice-vdagent], [0.17.0])
AC_CONFIG_SRCDIR([configure.ac])

AM_CONFIG_HEADER([src/config.h])

AM_INIT_AUTOMAKE([foreign dist-bzip2 no-dist-gzip subdir-objects])
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_PROG_CC
AM_PROG_CC_C_O
AC_HEADER_STDC
AC_SYS_LARGEFILE
AC_PROG_INSTALL
AC_PROG_LN_S
AC_DEFINE(_GNU_SOURCE, [1], [Enable GNU extensions])
PKG_PROG_PKG_CONFIG

AC_ARG_WITH([session-info],
  [AS_HELP_STRING([--with-session-info=@<:@auto/console-kit/systemd/none@:>@],
                  [Session-info source to use @<:@default=auto@:>@])],
  [case "$with_session_info" in
     auto|console-kit|systemd|none) ;;
     *) AC_MSG_ERROR([invalid session-info type specified]) ;;
   esac],
  [with_session_info="auto"])

dnl based on libvirt configure --init-script
AC_MSG_CHECKING([for init script flavor])
AC_ARG_WITH([init-script],
  [AC_HELP_STRING(
     [--with-init-script=@<:@redhat/systemd/systemd+redhat/check@:>@],
     [Style of init script to install @<:@default=check@:>@])],
     [],[with_init_script=check])
init_redhat=no
init_systemd=no
case "$with_init_script" in
    systemd+redhat)
       init_redhat=yes
       init_systemd=yes
       ;;
    systemd)
       init_systemd=yes
       ;;
    redhat)
       init_redhat=yes
       ;;
    none)
       ;;
    check)
       with_init_script=none
       if test "$cross_compiling" != yes && test -f /etc/redhat-release; then
          init_redhat=yes
          with_init_script=redhat
       fi
       ;;
    *)
       AC_MSG_ERROR([Unknown initscript flavour $with_init_script])
    ;;
esac
AM_CONDITIONAL([INIT_SCRIPT_RED_HAT], test "$init_redhat" = "yes")
AM_CONDITIONAL([INIT_SCRIPT_SYSTEMD], test "$init_systemd" = "yes")
AC_MSG_RESULT($with_init_script)

if test "x$init_systemd" = "xyes"; then
  SYSTEMDSYSTEMUNITDIR=`${PKG_CONFIG} systemd --variable=systemdsystemunitdir`
  AC_SUBST(SYSTEMDSYSTEMUNITDIR)
  # earlier versions of systemd require a separate libsystemd-daemon library
  PKG_CHECK_MODULES([LIBSYSTEMD_DAEMON],
                    [libsystemd >= 209],
                    [have_libsystemd_daemon="yes"],
                    [have_libsystemd_daemon="no"])
  if test "$have_libsystemd_daemon" = "yes"; then
      AC_DEFINE([WITH_SYSTEMD_SOCKET_ACTIVATION], [1], [If defined, vdagentd will support socket activation with systemd] )
  fi
fi

AC_ARG_ENABLE([pciaccess],
              [AS_HELP_STRING([--enable-pciaccess], [Enable libpciaccess use for auto generation of Xinerama xorg.conf (default: yes)])],
              [enable_pciaccess="$enableval"],
              [enable_pciaccess="yes"])

AC_ARG_ENABLE([static-uinput],
              [AS_HELP_STRING([--enable-static-uinput], [Enable use of a fixed, static uinput device for X-servers without hotplug support (default: no)])],
              [enable_static_uinput="$enableval"],
              [enable_static_uinput="no"])

PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.34])
PKG_CHECK_MODULES(X, [xfixes xrandr >= 1.3 xinerama x11])
PKG_CHECK_MODULES(SPICE, [spice-protocol >= 0.12.13])
PKG_CHECK_MODULES(ALSA, [alsa >= 1.0.22])
PKG_CHECK_MODULES([DBUS], [dbus-1])

if test "$with_session_info" = "auto" || test "$with_session_info" = "systemd"; then
    PKG_CHECK_MODULES([LIBSYSTEMD_LOGIN],
                      [libsystemd >= 209],
                      [have_libsystemd_login="yes"],
                      [have_libsystemd_login="no"])
    if test "$have_libsystemd_login" = "no"; then
        # for backward compatibility with older systemd versions
        PKG_CHECK_MODULES([LIBSYSTEMD_LOGIN],
                          [libsystemd-login >= 42],
                          [have_libsystemd_login="yes"],
                          [have_libsystemd_login="no"])
    fi
    if test x"$have_libsystemd_login" = "xno" && test "$with_session_info" = "systemd"; then
        AC_MSG_ERROR([libsystemd-login support explicitly requested, but some required packages are not available])
    fi
    if test x"$have_libsystemd_login" = "xyes"; then
        AC_DEFINE(HAVE_LIBSYSTEMD_LOGIN, [1], [If defined, vdagentd will be compiled with libsystemd-login support])
        with_session_info="systemd"
    fi
else
    have_libsystemd_login="no"
fi
AM_CONDITIONAL(HAVE_LIBSYSTEMD_LOGIN, test x"$have_libsystemd_login" = "xyes")

if test "$with_session_info" = "auto" || test "$with_session_info" = "console-kit"; then
    AC_DEFINE([HAVE_CONSOLE_KIT], [1], [If defined, vdagentd will be compiled with ConsoleKit support])
    have_console_kit="yes"
    with_session_info="console-kit"
else
    have_console_kit="no"
fi
AM_CONDITIONAL(HAVE_CONSOLE_KIT, test x"$have_console_kit" = "xyes")

if test x"$enable_pciaccess" = "xyes" ; then
    PKG_CHECK_MODULES(PCIACCESS, [pciaccess >= 0.10])
    AC_DEFINE([HAVE_PCIACCESS], [1], [If defined, vdagentd will be compiled with pciaccess support] )
fi
AM_CONDITIONAL(HAVE_PCIACCESS, test x"$enable_pciaccess" = "xyes")

if test x"$enable_static_uinput" = "xyes" ; then
    AC_DEFINE([WITH_STATIC_UINPUT], [1], [If defined, vdagentd will use a static uinput device] )
fi

# If no CFLAGS are set, set some sane default CFLAGS
if test "$ac_test_CFLAGS" != set; then
  DEFAULT_CFLAGS="-Wall -Werror -Wp,-D_FORTIFY_SOURCE=2 -fno-strict-aliasing -fstack-protector --param=ssp-buffer-size=4"
  for F in $DEFAULT_CFLAGS; do
    AC_MSG_CHECKING([whether $CC supports $F])
    save_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS $F"
    AC_COMPILE_IFELSE([AC_LANG_SOURCE([ ])], [cc_flag=yes], [cc_flag=no])
    if test "x$cc_flag" != "xyes"; then
      CFLAGS="$save_CFLAGS"
    fi
    AC_MSG_RESULT([$cc_flag])
  done
fi

AC_ARG_ENABLE([pie],
  AS_HELP_STRING([--enable-pie=@<:@auto/yes/no@:>@],
                 [Enable position-independent-executable support (for spice-vdagentd)@<:@default=auto@:>@]),
  [],
  [enable_pie="auto"])

if test "x$enable_pie" != "xno"; then
  save_CFLAGS="$CFLAGS"
  save_LDFLAGS="$LDFLAGS"
  CFLAGS="$CFLAGS -fPIE"
  LDFLAGS="$LDFLAGS -pie -Wl,-z,relro -Wl,-z,now"
  AC_MSG_CHECKING([for PIE support])
  AC_LINK_IFELSE([AC_LANG_SOURCE([int main () { return 0; }])],
                 [have_pie=yes],
                 [have_pie=no])
  AC_MSG_RESULT([$have_pie])
  if test "x$have_pie" = "xno" && test "x$enable_pie" = "xyes"; then
    AC_MSG_ERROR([pie support explicitly requested, but your toolchain does not support it])
  fi
  if test "x$have_pie" = "xyes"; then
    PIE_CFLAGS="-fPIE"
    PIE_LDFLAGS="-pie -Wl,-z,relro -Wl,-z,now"
    AC_SUBST(PIE_CFLAGS)
    AC_SUBST(PIE_LDFLAGS)
  fi
  CFLAGS="$save_CFLAGS"
  LDFLAGS="$save_LDFLAGS"
else
  have_pie=no
fi

AC_CONFIG_FILES([
Makefile
data/spice-vdagent.1
data/spice-vdagentd.1
])
AC_OUTPUT

dnl ==========================================================================
AC_MSG_NOTICE([

        spice-vdagent $VERSION
        ====================

        prefix:                   ${prefix}
        c compiler:               ${CC}

        session-info:             ${with_session_info}
        pciaccess:                ${enable_pciaccess}
        static uinput:            ${enable_static_uinput}
        vdagentd pie + relro:     ${have_pie}

        install RH initscript:    ${init_redhat}
        install systemd service:  ${init_systemd}

        Now type 'make' to build $PACKAGE

])
