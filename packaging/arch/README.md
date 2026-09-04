# Arch/Omarchy package

This directory builds a pacman-managed replacement for the official Arch
`libfprint` package. The resulting package contains every default upstream
driver plus the OMS/O-Film `33a7:2388` driver from this repository.

## Build

Run as the normal user, not as root:

```sh
./packaging/arch/build-package.sh
```

The script stages the current driver and integration patch, checks their hashes,
checks out the pinned upstream commit, runs the core libfprint tests, and writes
the package to `packaging/arch/out/`. If the project's upstream cache exists it
is reused; otherwise makepkg downloads libfprint from its official GitLab
repository.

## Package relationship

The package is named `libfprint-oms2388`. It provides `libfprint=1.94.100` and
`libfprint-2.so=2-64`, and conflicts with the repository `libfprint` package.
It intentionally does not use `replaces`: this is an alternative build, not a
permanent package rename. The repository `fprintd` package remains installed and
uses the unchanged public libfprint ABI.

## Install

Installation changes the system package set and requires root privileges:

```sh
sudo pacman -U packaging/arch/out/libfprint-oms2388-1.94.100-3-x86_64.pkg.tar.zst
sudo systemctl enable fprintd.service
sudo systemctl restart fprintd.service
```

After installation, reload udev state or reboot before testing the service. Do
not copy build artifacts directly into `/usr`; pacman must own the installed
files. The packaged systemd drop-in runs fprintd with `--no-timeout`; enabling
the service makes its sleep delay inhibitor available before a suspend request,
instead of racing with Omarchy's lock-on-suspend path. Restarting is intentional:
Polkit may have started fprintd during package installation with the old command
line, and `enable --now` alone does not restart an already active process.

## Upgrade maintenance

Because the installed package has a distinct name, a normal `pacman -Syu` does
not overwrite it merely because the repository publishes a newer `libfprint`.
If a future `fprintd` requires a newer libfprint version or SONAME, rebuild this
package on that newer upstream release before accepting the fprintd upgrade.

## Roll back

The official package is already present in the local pacman cache. The normal
rollback is:

```sh
sudo pacman -S libfprint
```

Pacman will ask to remove the conflicting `libfprint-oms2388` package. The
current cached official package can also be installed explicitly with
`pacman -U` if repository access is unavailable.
