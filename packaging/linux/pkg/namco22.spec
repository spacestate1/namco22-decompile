# Binary RPM around a tree staged by build-stage.sh:
#   rpmbuild -bb namco22.spec --define "_stage /path/to/stage" --define "_ver 0.1.0"
Name:           namco22
Version:        %{_ver}
Release:        1%{?dist}
Summary:        Prop Cycle, Rave Racer, Tokyo Wars, Dirt Dash, Time Crisis, Ace Driver, Cyber Commando and Cyber Sled -- decompiled Namco System 22 / 21 engines (Cyber Sled: System 21)
License:        MIT
ExclusiveArch:  x86_64
Recommends:     zenity
Recommends:     xdg-utils
%global debug_package %{nil}
%global __strip /bin/true

%description
Native, decompiled engines for the Namco arcade games Prop Cycle (1996),
Rave Racer (1995), Tokyo Wars (1996), Dirt Dash (1995), Time Crisis (1995), Ace Driver (1994), Cyber Commando (1994) and Cyber Sled (1993), rendered with OpenGL.

NO GAME DATA IS INCLUDED. After installing, put the MAME ROM sets in
~/.local/share/namco22/propcycle/roms/ (propcycl.zip) and
~/.local/share/namco22/raverace/roms/ (raverace.zip, namcoc74.zip) and
~/.local/share/namco22/tokyowar/roms/ (tokyowar.zip) and
~/.local/share/namco22/dirtdash/roms/ (dirtdash.zip) and
~/.local/share/namco22/timecris/roms/ (timecris.zip) and
~/.local/share/namco22/acedriver/roms/ (acedrive.zip, namcoc74.zip) and
~/.local/share/namco22/cybrcomm/roms/ (cybrcomm.zip, namcoc74.zip) and
~/.local/share/namco22/cybsled/roms/ (cybsled.zip, namcoc67.zip, namcoc68.zip).
See /usr/share/doc/namco22/README.txt.

%install
cp -a %{_stage}/usr %{buildroot}/

%post
cat /usr/share/doc/namco22/rom-note.txt
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
/usr/lib/namco22
/usr/share/applications/propcycle.desktop
/usr/share/applications/raveracer.desktop
/usr/share/applications/tokyowars.desktop
/usr/share/applications/dirtdash.desktop
/usr/share/applications/timecrisis.desktop
/usr/share/applications/acedriver.desktop
/usr/share/applications/cybercommando.desktop
/usr/share/applications/cybersled.desktop
/usr/share/icons/hicolor/256x256/apps/propcycle.png
/usr/share/icons/hicolor/256x256/apps/raveracer.png
/usr/share/icons/hicolor/256x256/apps/tokyowars.png
/usr/share/icons/hicolor/256x256/apps/dirtdash.png
/usr/share/icons/hicolor/256x256/apps/timecrisis.png
/usr/share/icons/hicolor/256x256/apps/acedriver.png
/usr/share/icons/hicolor/256x256/apps/cybercommando.png
/usr/share/icons/hicolor/256x256/apps/cybersled.png
%license /usr/share/doc/namco22/LICENSE
%doc /usr/share/doc/namco22/README.txt
%doc /usr/share/doc/namco22/rom-note.txt
