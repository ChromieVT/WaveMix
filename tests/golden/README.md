# Golden files

Reference behaviour that the tests check WaveMix against: channel IDs and names, how config files are loaded, the login file, and full `GetState` replies for a set of situations.

They pin down things users and other programs rely on, such as sink names in OBS scenes, saved levels, and the D-Bus replies. **Change them only on purpose**, in the same commit as the code change that needs it.

| File | What it pins down |
|---|---|
| `ids_and_names.json` | `make_id`, `valid_id`, sink and link names, constants, module argument strings |
| `config_loading.json` | How missing, broken and partial `config.json` / `state.json` files are loaded |
| `scenarios/*.json` | Full situations: input files → steps → `GetState`, sound-server calls and the files written |

## Scenario files

Each has:

- `input`: the `config.json` and `state.json` before the run (`null` = no file).
- `steps`: each action taken, with its `result` or the `dbus_error` and `message` a client gets, plus `server_calls`, what was asked of pipewire-pulse.
- `get_state`: the `GetState` JSON at the end.
- `output`: the `config.json`, `state.json` and `60-wavemix.conf` on disk at the end.

The stream and sink numbers in them are examples. On a real system they're PipeWire's `object.serial` numbers.

## Using them in tests

- Compare **parsed JSON**, not text. Key order doesn't matter.
- Compare volumes with a tolerance of `0.0001` (they're rounded to 4 decimals).
- `server_calls` describe *how* plumbing is created (`module_load`, `sink_input_move`). Tests of the PipeWire layer should check the **effects** instead: the right sinks and loopbacks exist, streams end up on the right sink, levels and mutes match.
