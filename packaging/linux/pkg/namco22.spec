# Binary RPM around a tree staged by build-stage.sh:
#   rpmbuild -bb namco22.spec --define "_stage /path/to/stage" --define "_ver 0.1.0"
Name:           namco22
Version:        %{_ver}
Release:        1%{?dist}
Summary:        Prop Cycle, Rave Racer, Tokyo Wars and Dirt Dash -- decompiled Namco System 22 engines
License:        LicenseRef-Not-Specified
ExclusiveArch:  x86_64
Recommends:     zenity
Recommends:     xdg-utils
%global debug_package %{nil}
%global __strip /bin/true

%description
Native, decompiled engines for the Namco arcade games Prop Cycle (1996),
Rave Racer (1995), Tokyo Wars (1996) and Dirt Dash (1995), rendered with OpenGL.

NO GAME DATA IS INCLUDED. After installing, put the MAME ROM sets in
~/.local/share/namco22/propcycle/roms/ (propcycl.zip) and
~/.local/share/namco22/raverace/roms/ (raverace.zip, namcoc74.zip) and
~/.local/share/namco22/tokyowar/roms/ (tokyowar.zip) and
~/.local/share/namco22/dirtdash/roms/ (dirtdash.zip). If a game says c71.bin is
missing, also put namcoc71.zip in its roms/ folder.
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
/usr/lib/namco22
/usr/share/applications/propcycle.desktop
/usr/share/applications/raveracer.desktop
/usr/share/applications/tokyowars.desktop
/usr/share/applications/dirtdash.desktop
/usr/share/icons/hicolor/256x256/apps/propcycle.png
/usr/share/icons/hicolor/256x256/apps/raveracer.png
/usr/share/icons/hicolor/256x256/apps/tokyowars.png
/usr/share/icons/hicolor/256x256/apps/dirtdash.png
%doc /usr/share/doc/namco22/README.txt
%doc /usr/share/doc/namco22/rom-note.txt
