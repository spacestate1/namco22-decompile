# Binary RPM around a tree staged by build-stage.sh:
#   rpmbuild -bb namco-2x-systems.spec --define "_stage /path/to/stage" --define "_ver 0.1.0"
Name:           namco-2x-systems
Version:        %{_ver}
Release:        1%{?dist}
Summary:        Prop Cycle, Rave Racer, Tokyo Wars, Dirt Dash, Time Crisis, Ace Driver, Cyber Commando, Cyber Sled and Time Crisis 2 -- decompiled Namco System 21 / 22 / 23 engines
License:        MIT
ExclusiveArch:  x86_64
Obsoletes:      namco22 < 0.7.0
Provides:       namco22 = %{version}-%{release}
Recommends:     zenity
Recommends:     xdg-utils
%global debug_package %{nil}
%global __strip /bin/true

%description
Native, decompiled engines for the Namco arcade games Prop Cycle (1996),
Rave Racer (1995), Tokyo Wars (1996), Dirt Dash (1995), Time Crisis (1995), Ace Driver (1994), Cyber Commando (1994), Cyber Sled (1993) and Time Crisis II (1997), rendered with OpenGL.

NO GAME DATA IS INCLUDED. After installing, put the MAME ROM sets in
~/.local/share/namco-2x-systems/propcycle/roms/ (propcycl.zip) and
~/.local/share/namco-2x-systems/raverace/roms/ (raverace.zip, namcoc74.zip) and
~/.local/share/namco-2x-systems/tokyowar/roms/ (tokyowar.zip) and
~/.local/share/namco-2x-systems/dirtdash/roms/ (dirtdash.zip) and
~/.local/share/namco-2x-systems/timecris/roms/ (timecris.zip) and
~/.local/share/namco-2x-systems/acedriver/roms/ (acedrive.zip, namcoc74.zip) and
~/.local/share/namco-2x-systems/cybrcomm/roms/ (cybrcomm.zip, namcoc74.zip) and
~/.local/share/namco-2x-systems/cybsled/roms/ (cybsled.zip, namcoc67.zip, namcoc68.zip) and
~/.local/share/namco-2x-systems/tc2/roms/ (timecrs2.zip).
Folders from an older namco22 install (~/.local/share/namco22/) are used as they are.
See /usr/share/doc/namco-2x-systems/README.txt.

%install
cp -a %{_stage}/usr %{buildroot}/

%post
cat /usr/share/doc/namco-2x-systems/rom-note.txt
exit 0

%files
/usr/bin/propcycle
/usr/bin/raveracer
/usr/bin/tokyowars
/usr/bin/dirtdash
/usr/bin/timecrisis
/usr/bin/acedriver
/usr/bin/cybercommando
/usr/bin/cybersled
/usr/bin/timecrisis2
/usr/lib/namco-2x-systems
/usr/share/applications/propcycle.desktop
/usr/share/applications/raveracer.desktop
/usr/share/applications/tokyowars.desktop
/usr/share/applications/dirtdash.desktop
/usr/share/applications/timecrisis.desktop
/usr/share/applications/acedriver.desktop
/usr/share/applications/cybercommando.desktop
/usr/share/applications/cybersled.desktop
/usr/share/applications/timecrisis2.desktop
/usr/share/icons/hicolor/256x256/apps/propcycle.png
/usr/share/icons/hicolor/256x256/apps/raveracer.png
/usr/share/icons/hicolor/256x256/apps/tokyowars.png
/usr/share/icons/hicolor/256x256/apps/dirtdash.png
/usr/share/icons/hicolor/256x256/apps/timecrisis.png
/usr/share/icons/hicolor/256x256/apps/acedriver.png
/usr/share/icons/hicolor/256x256/apps/cybercommando.png
/usr/share/icons/hicolor/256x256/apps/cybersled.png
/usr/share/icons/hicolor/256x256/apps/timecrisis2.png
%license /usr/share/doc/namco-2x-systems/LICENSE
%doc /usr/share/doc/namco-2x-systems/README.txt
%doc /usr/share/doc/namco-2x-systems/rom-note.txt
