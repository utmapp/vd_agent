Gitlab markdown format support linking to Issues (#) and Merge requests (!) and more, see bellow:

https://gitlab.freedesktop.org/spice/linux/vd_agent/-/blob/master/CHANGELOG.md

News in spice-vdagent 0.20.0
============================

* Add gio-unix and gobject dependency >= 2.50
* Bump gtk+ >= 3.22 (optional dependency)
* Last release with gtk+ being optional
* !4 - Race fixes between client and guest clipboard
* !2 - Fix session lookup for new GNOME versions
* !3 - Now using GMainLoop and GIO to handle I/O of messages
* Several minor covscan fixes

News in spice-vdagent 0.19.0
============================

* Add libdrm dependency
* Fix file descriptor leak on failed connections
* Handle new VD_AGENT_GRAPHICS_DEVICE_INFO message advertised by
  VD_AGENT_CAP_GRAPHICS_DEVICE_INFO capability
* Session agent autostart changed to WindowManager ([rhbz#1623947])
  This fixes possible race with xdg-user-dirs
* Fix of sending empty screen resolution messages ([rhbz#1641723])
* Fix 'Dependency failed for Activation socket' with systemd ([rhbz#1545212])
* Fix error messages about on selecting text on host ([rhbz#1594876])
  this was also fixed with Gtk backend
* Update paths from /var/run → /run
* Fix Session agent restart
* Add test for file creation
* Prefer GLib memory functions stdlib.h ones
* Several code and logs improvements

[rhbz#1623947]: https://bugzilla.redhat.com/show_bug.cgi?id=1623947
[rhbz#1641723]: https://bugzilla.redhat.com/show_bug.cgi?id=1641723
[rhbz#1545212]: https://bugzilla.redhat.com/show_bug.cgi?id=1545212
[rhbz#1594876]: https://bugzilla.redhat.com/show_bug.cgi?id=1594876

News in spice-vdagent 0.18.0
============================

* Add GTK+ framework to handle x11 backend such as clipboard
* Deprecate X11 backend in favor of GTK+ framework
* Ask pkg-config to appropriate directory to install udev rules
* Fix leak of udscs's file descriptor
* Better quote directory path when calling xdg-open to save file transfer
* Bump GLib to 2.34
* Add systemd socket activation (rhbz#1340160)
* Add support to detailed errors on file transfers
* Add check for available free space in guest before starting a file transfer
* Use better names for duplicated files on file transfer
* Improve support on big endian guests (#5)
* Use IdleHint to check if a session is locked over console-kit (rhbz#1412673)
* Fixes double free on errors over udscs by improving memory ownership
* Hide autostart file on Unity
* Improve command line messages for vdagentd
* Fix typo in --enable-static-uinput configure option
* Code repository moved to gitlab.freedesktop.org

News in spice-vdagent 0.17.0
============================

* Denies file-transfer in locked sessions
  * systems under systemd (rhbz#1323623)
  * systems under console-kit (rhbz#1323630)
* Denies file-transfer in login screen
  * systems under systemd (rhbz#1328761)
  * systems under console-kit (rhbz#1323640)
* Bump glib version to 2.28
* Set exit code to 1 instead of 0 when virtio device cannot be opened
* Fix double-free on uinput->screen_info (rhbz#1262635)
* Code improvement over unix domain client server support (udcs)
* Fix build compatiblity with different libsystemd versions (fdo#94209)

News in spice-vdagent 0.16.0
============================

* Add audio volume synchronization support
* Add support for maximum clipboard size
* Add support for more clipboard targets (STRING and TIMESTAMP)
* Improve handling of transfers of multiple files
* Fix transfer of >2GB files on 32 bit systems
* XSpice improvements
* Various bug fixes related to resolution changes

News in spice-vdagent 0.15.0
============================

* Xspice support
* Release clipboard on client disconnect if owned by client (rhbz#1003977)
* Turn some error messages into debugging messages (rhbz#918310)
* Not having the virtio channel is not an error; instead silently do nothing

News in spice-vdagent 0.14.0
============================

* More multi-monitor and arbritary resolution support bugfixes
* Add support for file transfers from client to guest
* Add support for setups with multiple Screens (multiple qxl devices each
  mapped to their own screen), limitations:
  * Max one monitor per Screen / qxl device
  * All monitors / Screens must have the same resolution
  * No client -> guest resolution syncing
* Add spice-vdagent -X cmdline option, which runtime disables console-kit /
  systemd-logind integration for setups where these are not used
* Add manpages for spice-vdagent and spice-vdagentd

News in spice-vdagent 0.12.1
============================

* Various bugfixes for multi-monitor and arbritary resolution support
* Requires libXrandr >= 1.3, Note 0.12.0 also required this, but did not
  check for it. For older distributions use 0.10.1

News in spice-vdagent 0.12.0
============================

* Full multi-monitor and arbritary resolution support, this requires a new
  enough xorg-x11-drv-qxl driver, as well as a new enough host
* systemd service support, using systemd hardware acivation
* Use syslog for logging, rather then logging to private log files

News in spice-vdagent 0.10.1
============================

* Fix a race condition when opening the vdagent virtio serial port, which
  caused it to get opened / closed in rapid succession a number of times
  on vm boot

News in spice-vdagent 0.10.0
============================

* Add limited support for multiple displays, see README
* Add support for RHEL-5 (and other distributions with a non hotplug
  capable Xorg and/or no console-kit), see README.RHEL-5
* Add support for using libsystemd-logind as session information source
  instead of console-kit

News in spice-vdagent 0.8.1
===========================

* In daemon mode the session vdagent now retries connecting to the system
  vdagentd every second, once a connection is made a version check is done,
  if the version differs (which only happens on an upgrade from one version
  to the next) the sesion vdagent re-execs itself (Marc-André Lureau)

News in spice-vdagent 0.8.0
===========================

* Add support for copy and paste using the primary selection, to use this
  you need a spice-gtk widget based client and the latest spice-gtk code
  (Marc-André Lureau and Hans de Goede)
* Autotoolized (Christophe Fergeau)
* Allow building without consolekit, for systems which don't have ck, such
  as RHEL-5 (Christophe Fergeau)
* Various small bugfixes (Hans de Goede)

News in spice-vdagent 0.6.3
===========================

* Initial release, starting with version nr 0.6.3, to indicate that it
  more or less supports all parts of the cdagent protocol in spice-protocol
  and spice 0.6.3
