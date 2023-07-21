# matcha

An Idle Inhibitor for Wayland

## Why?

Some Desktop Environments and status bars may not include an idle inhibitor.

Matcha implements a general Wayland idle inhibitor that utilize wayland-protocols as oppossed
to the common approach of using dbus.


## Usage

```
  Usage: matcha [OPTION]...
  Options:
    -m, --bar=[BAR]    Set the bar type to bar (default: None)
    -o, --off          Run main instance with inhibitor disabled
    -t, --toggle       Toggle the inhibit state
    -h, --help         Display this help and exit

  BAR:
    Yambar - Only works on main instance
    Waybar - Only works on toggle instance
```

Matcha consists of two parts, a main instance that runs if `--toggle` flag is not set, and a toggle instance.

The main instance simply initialize and starts the inhibitor while waiting possible toggle.
The toggle instance, toggles the state and exits (with a possible message).

For example, `matcha` can be used as a part of yambar:

```yml
bar:
  height: 42
  location: top
  background: 3E3F67ff
  font: Ubuntu Nerd Font:pixelsize=16
  right:
    - script:
        path: matcha # Path to matcha executable
        args: ['--bar=yambar', '--off']
        content:
            - map:
                on-click:
                  left: matcha --toggle
                conditions:
                  inhibit: { string: { text: "🍵" , right-margin: 5} }
                  ~inhibit: { string: { text: "💤", right-margin: 5} }
            - string: { text: "|", right-margin: 5}
```

Matcha also allows using Waybar if you prefer using it instead of the builtin Waybar idle-inhibitor module.

This would involve running matcha main instance, then adding a toggle "custom" with `matcha --toggle -- bar=waybar`.

This should provide waybar with a formatted output. To modify the formatted output,
modify the `MATCHA_WAYBAR_OFF`,`MATCHA_WAYBAR_ON` environment variables with the text/emoji you would like be shown.



## Building from Source

### Requirements:

- wayland-client
- meson
- a C23 compiler

```bash
  $ meson setup build --buildtype=release -Dprefix=$INSTALL_LOCATION
  $ meson compile -C build
  $ meson install -C build
```
