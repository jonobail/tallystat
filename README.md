# tallystat

A terminal system monitor for Ubuntu with braille graphs, written in C with no dependencies beyond libc.

It shows:

- CPU usage per core, or the average, as braille line graphs
- Main and swap memory usage graphs
- Network rx/tx sparklines, totals, and current rates
- Disk usage and read/write rates for each mounted device
- Temperatures from every hwmon sensor
- A process list you can sort, filter, group by name, and kill from

## Install

### From a release `.deb`

```sh
sudo apt-get install ./tallystat_1.0.0_amd64.deb
```

### From a local apt repository

```sh
make repo
echo "deb [trusted=yes] file:$PWD/dist/apt ./" | sudo tee /etc/apt/sources.list.d/tallystat.list
sudo apt-get update
sudo apt-get install tallystat
```

`make repo` builds the package with `dpkg-deb` and indexes it with
`dpkg-scanpackages`. Both come with `dpkg-dev`.

### From source

```sh
make
sudo make install          # /usr/local/bin/tallystat
```

The `debian/` directory is a standard debhelper source package. Use it to build with
`dpkg-buildpackage -us -uc` or to upload to a Launchpad PPA.

## Usage

```
tallystat [options]

  -c, --color=NAME        default, ember, monokai, solarized, nord
  -m, --minimal           only CPU, memory and processes
  -r, --rate=SECONDS      refresh interval (default 1)
  -a, --averagecpu        show the average CPU line
  -p, --percpu            show a line per core
  -s, --statusbar         hostname, clock and uptime
  -f, --fahrenheit        temperatures in °F
  -S, --graphscale=N      horizontal graph scale 1-40 (default 7)
  -i, --interface=NAME    only measure one network interface
```

Press `?` inside tallystat to see all key bindings. The main ones:

| Key | Action |
| --- | --- |
| `q` / `Ctrl-c` | quit |
| `j` `k` / arrows | move through processes |
| `gg` `G` / Home End | jump to the top or bottom |
| `Tab` | group processes by name |
| `dd` | kill the selected process or group (SIGTERM) |
| `/` | filter by name or PID |
| `c` `m` `p` | sort by CPU, memory, or PID |
| `h` `l` | scale graphs in or out |

## License

MIT
