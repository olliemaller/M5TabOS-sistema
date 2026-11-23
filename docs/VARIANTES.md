# Variants: one system, more than one binary

M5tabOS is **one code base** that produces more than one firmware. The core
(kernel, compositor, services, HAL, console) exists once: a fix made there
applies to every variant, because it is the same file. Variants only change
**what goes into** the binary - never the system code.

| variant | what it has | what for | flash with |
|---|---|---|---|
| **completa** (full, default) | every C app, including personal and lab ones | everyday use | `./gravar.sh` |
| **sistema** | only the system apps | the public base (the GitHub copy, `ferramentas/publicar.sh`) | `./gravar.sh sistema` |
| **m5burner** | a copy of **berry**: the system apps + Berry, with the example app `Ola.be` installed at first boot | the M5Burner firmware (`ferramentas/m5burner.sh`), raw: flashing erases the settings | `./gravar.sh m5burner` |
| **berry** | the system apps + apps in [Berry](BERRY.md) (`.be` on the card, no compiling) | installing apps without compiling | `./gravar.sh berry` |

**No commands to memorise:** `./gravar.sh` with nothing (or the VS Code
**Gravar** button) asks which variant to flash - Enter repeats the last one.
For everything else (build them all, M5Burner, release a version, publish to
GitHub) there is the menu `ferramentas/menu.sh`: double-click
**`M5tabOS.command`** in the Finder, or the VS Code **Menu** button.

Each one builds in its own folder: `build/` (full), `build_<variant>/`, and
`_v3` at the end for the P4 v3.x.

The flashed version says which it is: `0.8.0` and `0.8.0-berry` (in Settings,
on the console and in the boot log).

## How a variant is made

A file `variantes/<name>.cmake` with four lines:

```cmake
set(M5_BERRY ON)                 # does Berry go into the binary?
set(M5_SO_SISTEMA ON)            # only the system apps (personal ones are left out by themselves)?
set(M5_SEM_APPS_VARIANTE "")     # and these apps left out too ("cobra;radar")
set(M5_VARIANTE_ROTULO "berry")  # the version suffix ("" = none)
```

The rest is automatic:

- `M5_BERRY OFF`: the `components/berry` component is not even built, the
  `main/services/berry_*` sources leave the build, and `M5_COM_BERRY` is 0.
- Left-out apps (those in `M5_SEM_APPS_VARIANTE` and, with `M5_SO_SISTEMA`,
  every app not in the `m5_apps_sistema` list in `main/CMakeLists.txt`) leave
  the sources **and** the app list - the folder stays in the repository for
  the other variants.
- Each variant builds in its own folder (`build_<variant>/`, `_v3` for the
  P4 v3), with its `sdkconfig` inside.

## The boundary: the core does not know Berry exists

Berry plugs into the system through **one generic extension point**, the
"outside apps" (`kernel/app.h`):

```c
void sistema_apps_de_fora_iniciar(void);   // boot: register the apps on the card
bool sistema_apps_de_fora_atender(void);   // home: apply a reload
void sistema_apps_de_fora_reler(void);     // the card changed (flash-drive mode)
```

**One file** implements them, `main/services/apps_de_fora.cpp`: in the berry
variant it calls the Berry apps; in the full variant the three functions do
nothing. `core.cpp` and `tab5_pendrive.cpp` call these functions and nothing
else. The console's `berry` command is the only other piece with
`#if M5_COM_BERRY`.

**The rule that keeps variants from drifting apart:** whatever belongs to the
system goes into the core, never inside `services/berry_*`. An improvement
found because of Berry (faster text, pasting straight to the screen, the
serial port coming back after flash-drive mode) belongs to the system - and
every variant gets it.

## The golden rule: every commit builds them all

```bash
ferramentas/compilar_tudo.sh
```

Builds every variant (with `v3`, also the P4 v3.x ones) and checks the example
Berry apps (`testes/checar_be.sh`). A commit is only ready when **everything
built**. A core change that breaks one variant breaks the system.

## A new variant

1. Create `variantes/<name>.cmake` (copy an existing one).
2. `ferramentas/compilar_tudo.sh` starts building it by itself.
3. If it turns on a new optional piece (like Berry), the piece plugs in
   through a core extension point, like the outside apps - never with `#if`
   scattered through the kernel.
