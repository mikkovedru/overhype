#!/usr/bin/env python3
"""Build a self-contained Qt .deb for Ubuntu 24.04 and Linux Mint 22.x."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
QML_MODULES = (
    "QML", "QtCore", "QtQml", "QtQml/Models", "QtQml/WorkerScript",
    "QtQuick", "QtQuick/Window", "QtQuick/Layouts", "QtQuick/Shapes",
    "QtQuick/Effects", "QtQuick/Templates", "QtQuick/Controls",
    "QtQuick/Controls/impl", "QtQuick/Controls/Basic",
    "QtQuick/Controls/Basic/impl", "QtMultimedia", "Qt/labs/qmlmodels",
)
PLUGIN_GROUPS = (
    "imageformats", "multimedia", "platforminputcontexts", "tls",
    "wayland-decoration-client", "wayland-graphics-integration-client",
    "wayland-shell-integration", "xcbglintegrations",
)
PLATFORM_PLUGINS = (
    "libqxcb.so", "libqwayland-egl.so", "libqwayland-generic.so",
    "libqoffscreen.so", "libqminimal.so",
)
FONTS = (
    ("fonts-jetbrains-mono", "jetbrains-mono", "JetBrainsMono-Regular.ttf"),
    ("fonts-jetbrains-mono", "jetbrains-mono", "JetBrainsMono-Bold.ttf"),
    ("fonts-jetbrains-mono", "jetbrains-mono", "JetBrainsMono-Italic.ttf"),
    ("fonts-jetbrains-mono", "jetbrains-mono", "JetBrainsMono-BoldItalic.ttf"),
    ("fonts-noto-core", "noto", "NotoSans-Regular.ttf"),
    ("fonts-noto-core", "noto", "NotoSans-Bold.ttf"),
    ("fonts-noto-mono", "noto", "NotoSansMono-Regular.ttf"),
    ("fonts-noto-mono", "noto", "NotoSansMono-Bold.ttf"),
)


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def fail(message):
    raise SystemExit(message)


def qt_tool():
    if os.environ.get("HYPE_QMAKE"):
        selected = os.environ["HYPE_QMAKE"]
        if "/" not in selected:
            selected = shutil.which(selected) or fail(f"Cannot find {selected} on PATH")
        return Path(selected).resolve()
    candidates = list((BUILD / "qt").glob("*/gcc_64/bin/qmake6"))
    if candidates:
        return sorted(candidates, key=lambda path: tuple(map(int, re.findall(r"\d+", path.parts[-4]))))[-1]
    found = shutil.which("qmake6")
    if not found:
        fail("Qt 6.9 or newer is required; set HYPE_QMAKE to its qmake6.")
    return Path(found).resolve()


def needed(path):
    output = run("readelf", "-d", str(path))
    return re.findall(r"\(NEEDED\).*?\[(.*?)\]", output)


def soname(path):
    output = run("readelf", "-d", str(path))
    match = re.search(r"\(SONAME\).*?\[(.*?)\]", output)
    return match.group(1) if match else ""


def copy_module(source, target):
    if not (source / "qmldir").is_file():
        fail(f"Missing Qt QML module: {source}")
    target.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        if item.is_file():
            shutil.copy2(item, target / item.name)


def copy_runtime(qt_prefix, appdir):
    qt_lib = Path(run(str(qt_prefix / "bin/qmake6"), "-query", "QT_INSTALL_LIBS"))
    qt_qml = Path(run(str(qt_prefix / "bin/qmake6"), "-query", "QT_INSTALL_QML"))
    qt_plugins = Path(run(str(qt_prefix / "bin/qmake6"), "-query", "QT_INSTALL_PLUGINS"))
    libdir = appdir / "lib"
    libdir.mkdir(parents=True)
    seeds = [appdir / "bin/hype"]
    for module in QML_MODULES:
        source = qt_qml / module
        target = appdir / "qml" / module
        copy_module(source, target)
        seeds.extend(target.glob("*.so"))
    for group in PLUGIN_GROUPS:
        source = qt_plugins / group
        if not source.is_dir():
            fail(f"Missing Qt plugin group: {source}")
        target = appdir / "plugins" / group
        target.mkdir(parents=True, exist_ok=True)
        for plugin in source.glob("*.so"):
            shutil.copy2(plugin, target / plugin.name)
            seeds.append(target / plugin.name)
    platform_dir = appdir / "plugins/platforms"
    platform_dir.mkdir(parents=True, exist_ok=True)
    for name in PLATFORM_PLUGINS:
        source = qt_plugins / "platforms" / name
        if not source.is_file():
            fail(f"Missing Qt platform plugin: {source}")
        shutil.copy2(source, platform_dir / name)
        seeds.append(platform_dir / name)
    # The app uses generic platform theming, but Qt's portal plugin also makes
    # desktop integration available to Qt dialogs used by its dependencies.
    portal = qt_plugins / "platformthemes/libqxdgdesktopportal.so"
    if portal.is_file():
        target = appdir / "plugins/platformthemes" / portal.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(portal, target)
        seeds.append(target)

    queue = list(seeds)
    visited = set()
    while queue:
        binary = queue.pop()
        if binary in visited:
            continue
        visited.add(binary)
        for name in needed(binary):
            source = qt_lib / name
            if not source.exists():
                if name.startswith(("libQt6", "libicu")):
                    fail(f"Missing private Qt dependency {name} required by {binary}")
                continue
            resolved = source.resolve()
            target = libdir / resolved.name
            if not target.exists():
                shutil.copy2(resolved, target)
                queue.append(target)
            if name != resolved.name:
                alias = libdir / name
                if not alias.exists():
                    alias.symlink_to(resolved.name)
    return seeds, libdir


def copy_fonts(appdir, docdir):
    roots = [Path(os.environ["HYPE_FONT_ROOT"])] if os.environ.get("HYPE_FONT_ROOT") else []
    if (BUILD / "deps/fontroot").is_dir():
        roots.append(BUILD / "deps/fontroot")
    roots.append(Path("/"))
    copied_licenses = set()
    for package, directory, filename in FONTS:
        for root in roots:
            source = root / "usr/share/fonts/truetype" / directory / filename
            if source.is_file():
                target = appdir / "fonts" / filename
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                if package not in copied_licenses:
                    license_file = root / "usr/share/doc" / package / "copyright"
                    if not license_file.is_file():
                        fail(f"Missing license for {source}: {license_file}")
                    shutil.copy2(license_file, docdir / f"copyright.{package}")
                    copied_licenses.add(package)
                break
        else:
            fail(f"Missing bundled font {filename}; install {package} or set HYPE_FONT_ROOT")


def package_dependencies(stage, seeds, libdir):
    workspace = BUILD / "deb/shlibdeps"
    (workspace / "debian").mkdir(parents=True, exist_ok=True)
    (workspace / "debian/control").write_text(
        "Source: hype\nSection: graphics\nPriority: optional\n"
        "Maintainer: Hype contributors <noreply@github.com>\nStandards-Version: 4.7.0\n\n"
        "Package: hype\nArchitecture: any\nDescription: Markdown presentation editor\n"
    )
    # Some merged-/usr systems expose libbz2 through an unowned /lib alias.
    # Its normal package relationship is still libbz2-1.0.
    shlibs = ["libbz2 1.0 libbz2-1.0"]
    private_binaries = sorted(path for path in libdir.iterdir() if path.is_file() and not path.is_symlink())
    for binary in private_binaries:
        name = soname(binary)
        match = re.fullmatch(r"(lib[^.]+)\.so\.(\d+)(?:\..*)?", name)
        if match:
            shlibs.append(f"{match.group(1)} {match.group(2)} hype")
    (workspace / "debian/shlibs.local").write_text("\n".join(sorted(set(shlibs))) + "\n")
    command = ["dpkg-shlibdeps", "-O", "-xhype", "--ignore-missing-info", f"-S{stage}",
               f"-L{workspace / 'debian/shlibs.local'}", f"-l{libdir}"]
    command += [f"-e{binary}" for binary in seeds + private_binaries]
    result = subprocess.run(command, cwd=workspace, text=True, capture_output=True)
    if result.returncode:
        fail("dpkg-shlibdeps failed:\n" + "\n".join(result.stderr.splitlines()[-12:]))
    missing_info = [line for line in result.stderr.splitlines()
                    if "no dependency information found" in line and "libbz2.so.1" not in line]
    if missing_info:
        fail("Missing system library package metadata:\n" + "\n".join(missing_info))
    output = result.stdout
    match = re.search(r"shlibs:Depends=(.*)", output)
    if not match:
        fail(f"dpkg-shlibdeps returned no dependencies: {output}")
    dependencies = set(match.group(1).split(", "))
    # This builder can run on Mint systems with third-party graphics packages.
    # The bundled runtime was smoke-tested against the stock Ubuntu 24.04
    # versions of these same SONAMEs, so describe that portable floor instead.
    portable = {
        "libdrm2-amdgpu": "libdrm2 (>= 2.4.120)",
        "libzstd1": "libzstd1 (>= 1.5.5)",
        "libwebpdemux2": "libwebpdemux2 (>= 1.3.2)",
    }
    dependencies = {
        portable.get(dependency.split(" (")[0], dependency)
        for dependency in dependencies
    }
    dependencies.update(("ffmpeg", "source-highlight", "xdg-desktop-portal", "libbz2-1.0"))
    return ", ".join(sorted(dependencies))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true", help="package an existing build/hype")
    args = parser.parse_args()
    qmake = qt_tool()
    version = run(str(qmake), "-query", "QT_VERSION")
    if tuple(map(int, version.split(".")[:2])) < (6, 9):
        fail(f"Qt 6.9 or newer is required; {qmake} reports {version}")
    qt_prefix = Path(run(str(qmake), "-query", "QT_INSTALL_PREFIX"))
    if not args.no_build:
        subprocess.run([str(ROOT / "bin/build")], check=True, env={**os.environ, "HYPE_QMAKE": str(qmake)})
    binary = BUILD / "hype"
    if not binary.is_file():
        fail("Missing build/hype; run bin/build first")
    arch = run("dpkg", "--print-architecture")
    app_version = re.search(r'app\.setApplicationVersion\("([^"]+)"\)', (ROOT / "src/main.cpp").read_text())
    if not app_version:
        fail("Cannot read application version from src/main.cpp")
    package_version = app_version.group(1) + "-1"
    stage = BUILD / "deb/stage"
    if stage.exists():
        shutil.rmtree(stage)
    appdir = stage / "opt/hype"
    (appdir / "bin").mkdir(parents=True)
    shutil.copy2(binary, appdir / "bin/hype")
    (appdir / "bin/qt.conf").write_text("[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\nQmlImports=qml\n")
    launcher = stage / "usr/bin/hype"
    launcher.parent.mkdir(parents=True)
    launcher.write_text(
        "#!/bin/sh\n"
        "appdir=${HYPE_APPDIR:-/opt/hype}\n"
        "export LD_LIBRARY_PATH=\"$appdir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
        "export QT_PLUGIN_PATH=\"$appdir/plugins\"\n"
        "export QML_IMPORT_PATH=\"$appdir/qml\"\n"
        "export QML2_IMPORT_PATH=\"$appdir/qml\"\n"
        "exec \"$appdir/bin/hype\" \"$@\"\n"
    )
    launcher.chmod(0o755)
    docdir = stage / "usr/share/doc/hype"
    docdir.mkdir(parents=True)
    shutil.copy2(ROOT / "LICENSE", docdir / "copyright.hype")
    shutil.copy2(ROOT / "src/themes/LICENSE.omarchy", docdir / "copyright.omarchy")
    lgpl = Path("/usr/share/common-licenses/LGPL-3")
    if lgpl.is_file():
        shutil.copy2(lgpl, docdir / "copyright.qt.LGPL-3")
    ffmpeg_license = Path("/usr/share/common-licenses/LGPL-2.1")
    if ffmpeg_license.is_file():
        shutil.copy2(ffmpeg_license, docdir / "copyright.ffmpeg.LGPL-2.1")
    (docdir / "Qt-runtime.txt").write_text(
        f"Bundled Qt runtime: {version}\n"
        f"Qt source: https://download.qt.io/official_releases/qt/{'.'.join(version.split('.')[:2])}/{version}/single/\n"
        "Qt's FFmpeg multimedia backend includes FFmpeg shared libraries. FFmpeg source: https://ffmpeg.org/download.html\n"
        "Qt libraries and plugins are separate, replaceable shared objects under /opt/hype/lib and /opt/hype/plugins.\n"
    )
    copy_fonts(appdir, docdir)
    seeds, libdir = copy_runtime(qt_prefix, appdir)
    desktop = stage / "usr/share/applications/hype.desktop"
    desktop.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "pkgbuild/hype.desktop", desktop)
    icon = stage / "usr/share/icons/hicolor/scalable/apps/hype.svg"
    icon.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "pkgbuild/hype.svg", icon)
    control = stage / "DEBIAN/control"
    control.parent.mkdir(parents=True)
    control.write_text("Package: hype\nVersion: " + package_version + "\nArchitecture: " + arch +
                       "\nDescription: Markdown presentation editor\n")
    dependencies = package_dependencies(stage, seeds, libdir)
    control.write_text(
        f"Package: hype\nVersion: {package_version}\nArchitecture: {arch}\n"
        "Section: graphics\nPriority: optional\n"
        "Maintainer: Hype contributors <noreply@github.com>\n"
        "Homepage: https://github.com/omacom/hype\n"
        f"Depends: {dependencies}\n"
        "Recommends: xdg-desktop-portal-xapp | xdg-desktop-portal-gtk\n"
        "Description: Markdown presentation editor with bundled Qt runtime\n"
        " Hype edits Markdown slides and exports PDF and PowerPoint presentations.\n"
    )
    output = BUILD / "deb" / f"hype_{package_version}_{arch}.deb"
    subprocess.run(["dpkg-deb", "--build", "--root-owner-group", str(stage), str(output)], check=True)
    print(output)


if __name__ == "__main__":
    main()
