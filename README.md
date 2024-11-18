Spice agent for Linux and macOS
===============================

### Linux

The spice agent for Linux consists of 2 parts, a daemon spice-vdagentd and
a per X-session process spice-vdagent. The daemon gets started in Spice guests
through a Sys-V initscript or a systemd unit. The per X-session gets
automatically started in desktop environments which honor /etc/xdg/autostart,
and under gdm.

The main daemon needs to know which X-session daemon is in the currently
active X-session (think switch user functionality) for this console kit or
systemd-logind (compile time option) is used. If no session info is
available only one X-session agent is allowed.

Features:
* Client mouse mode (no need to grab mouse by client, no mouse lag)
  this is handled by the daemon by feeding mouse events into the kernel
  via uinput. This will only work if the active X-session is running a
  spice-vdagent process so that its resolution can be determined.
* Automatic adjustment of the X-session resolution to the client resolution
* Support of copy and paste (text and images) between the active X-session
  and the client. This supports both the primary selection and the clipboard.
* Support for transferring files from the client to the agent
* Full support for multiple displays using Xrandr, this requires a new
  enough xorg-x11-drv-qxl driver, as well as a new enough host.
* Limited support for multiple displays using Xinerama.
* Limited support for setups with multiple Screens (multiple qxl devices each
  mapped to their own screen)

### macOS

Similar to Linux, the SPICE agent has a LaunchDaemon spice-vdagentd which
handles communication with the console port and a LaunchAgent spice-vdagent
that runs for each logged in user. Note that there is no protection against
one logged in user from snooping another user's clipboard when this is running.

The macOS agent currently only supports clipboard features.

## Install (Linux)

From inside your virtual machine (e.g., GNOME Boxes), use your guest system’s
package manager to install.

For example, if you’re running a Debian/Ubuntu derivative in a VM, use:

```shell
sudo apt install spice-vdagent
```

## Build (macOS)

Currently, you can only build on Apple Silicon Macs with Homebrew installed in
both native and Rosetta. This is because we require GLib installed in both
architectures at the default location.

>>>
    $ /opt/homebrew/bin/brew install glib # arm64
    $ /usr/local/bin/brew install glib # x86_64
>>>

Then, you can build the project with Xcode or `xcodebuild`

>>>
    $ xcodebuild archive -scheme vd_agent -archivePath vd_agent.xcarchive
>>>

Afterwards, you can sign the output and create an installer package with a paid
Apple Developer certificate (Developer ID Application and Developer ID
Installer are both needed).

>>>
    $ ./package_macos.sh vd_agent.xcarchive spice-vdagent-x.yy.pkg
>>>

You may then wish to notarize it before distributing.

## How it works

All vdagent communications on the guest side run over a single pipe which
gets presented to the guest os as a virtio serial port.

Under windows this virtio serial port has the following name:
>>>
    \\\\.\\Global\\com.redhat.spice.0
>>>

Under Linux this virtio serial port has the following name:
>>>
    /dev/virtio-ports/com.redhat.spice.0
>>>

Under macOS this virtio serial port has the following name:
>>>
    /dev/tty.com.redhat.spice.0
>>>

To enable the virtio serial port you need to pass the following params on
the qemu cmdline:

>>>
    -device virtio-serial-pci,id=virtio-serial0,max_ports=16,bus=pci.0,addr=0x5 \
    -chardev spicevmc,name=vdagent,id=vdagent \
    -device virtserialport,nr=1,bus=virtio-serial0.0,chardev=vdagent,name=com.redhat.spice.0
>>>
